#include "log.hpp"
#include "rang.hpp"

#include <cctype>
#include <cxxabi.h>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/SetVector.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Metadata.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Value.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Passes/PassPlugin.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include <stdexcept>
#include <z3++.h>

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm;

// TODO: consider constraints from annotation;

constexpr const char* MKINT_IR_TAINT = "mkint.taint";
constexpr const char* MKINT_IR_SINK = "mkint.sink";
constexpr const char* MKINT_IR_ERR = "mkint.err";
constexpr const char* MKINT_TAINT_SRC_SUFFX = ".mkint.arg";

static std::string demangle(const char* name)
{
    int status = -1;
    std::unique_ptr<char, void (*)(void*)> res { abi::__cxa_demangle(name, NULL, NULL, &status), std::free };
    return (status == 0) ? res.get() : std::string(name);
}

template <typename V, typename... Vs> static constexpr std::array<V, sizeof...(Vs)> mkarray(Vs&&... vs) noexcept
{
    return std::array<V, sizeof...(Vs)> { vs... };
}

constexpr auto MKINT_SINKS = mkarray<std::pair<const char*, size_t>>(std::pair { "malloc", 0 },
    std::pair { "__mkint_sink0", 0 }, std::pair { "__mkint_sink1", 1 }, std::pair { "__mkint_sink2", 2 },
    std::pair { "xmalloc", 0 }, std::pair { "kmalloc", 0 }, std::pair { "kzalloc", 0 }, std::pair { "vmalloc", 0 },
    std::pair { "mem_alloc", 0 }, std::pair { "__page_empty", 0 }, std::pair { "agp_alloc_page_array", 0 },
    std::pair { "copy_from_user", 2 }, std::pair { "__writel", 0 }, std::pair { "access_ok", 2 },
    std::pair { "btrfs_lookup_first_ordered_extent", 1 }, std::pair { "sys_cfg80211_find_ie", 2 },
    std::pair { "gdth_ioctl_alloc", 1 }, std::pair { "sock_alloc_send_skb", 1 }, std::pair { "memcpy", 2 });

struct crange : public ConstantRange {
    /// https://llvm.org/doxygen/classllvm_1_1ConstantRange.html
    using ConstantRange::ConstantRange;

    crange(uint32_t bw) // by default we assume it's full set.
        : ConstantRange(bw, true)
    {
    }

    crange(const ConstantRange& cr)
        : ConstantRange(cr)
    {
    }

    crange()
        : ConstantRange(0, true)
    {
    }

    static constexpr auto cmpRegion()
    {
        // makeAllowedICmpRegion: many false positives.
        return ConstantRange::makeAllowedICmpRegion;
        // makeSatisfyingICmpRegion: might miss some true positives.
        // return ConstantRange::makeSatisfyingICmpRegion;
    }
};

int64_t get_id(Instruction* inst)
{
    std::string str;
    llvm::raw_string_ostream rso(str);
    inst->print(rso);
    auto inst_str = StringRef(rso.str());
    auto ids = inst_str.trim().split(' ').first;
    size_t v = 0;
    for (auto c : ids) {
        if (!std::isdigit(c))
            continue;
        v = v * 10 + c - '0';
    }
    return v;
}

namespace {

using bbrange_t = DenseMap<const BasicBlock*, DenseMap<const Value*, crange>>;

enum class interr { OVERFLOW, DIV_BY_ZERO, BAD_SHIFT, ARRAY_OOB, DEAD_TRUE_BR, DEAD_FALSE_BR };

template <interr err, typename StrRet = const char*> constexpr StrRet mkstr()
{
    if constexpr (err == interr::OVERFLOW) {
        return "integer overflow";
    } else if (err == interr::DIV_BY_ZERO) {
        return "divide by zero";
    } else if (err == interr::BAD_SHIFT) {
        return "bad shift";
    } else if (err == interr::ARRAY_OOB) {
        return "array index out of bound";
    } else if (err == interr::DEAD_TRUE_BR) {
        return "impossible true branch";
    } else if (err == interr::DEAD_FALSE_BR) {
        return "impossible false branch";
    } else {
        static_assert(err == interr::OVERFLOW || err == interr::DIV_BY_ZERO || err == interr::BAD_SHIFT
                || err == interr::ARRAY_OOB || err == interr::DEAD_TRUE_BR || err == interr::DEAD_FALSE_BR,
            "unknown error type");
        return ""; // statically impossible
    }
}

std::string_view mkstr(interr err)
{
    switch (err) {
    case interr::OVERFLOW:
        return mkstr<interr::OVERFLOW>();
    case interr::DIV_BY_ZERO:
        return mkstr<interr::DIV_BY_ZERO>();
    case interr::BAD_SHIFT:
        return mkstr<interr::BAD_SHIFT>();
    case interr::ARRAY_OOB:
        return mkstr<interr::ARRAY_OOB>();
    case interr::DEAD_TRUE_BR:
        return mkstr<interr::DEAD_TRUE_BR>();
    case interr::DEAD_FALSE_BR:
        return mkstr<interr::DEAD_FALSE_BR>();
    default:
        break;
    }

    MKINT_CHECK_ABORT(false) << "unknown error type" << static_cast<int>(err);
    return ""; // statically impossible
}

template <interr err_t, typename I> static std::enable_if_t<std::is_pointer_v<I>> mark_err(I inst)
{
    auto& ctx = inst->getContext();
    auto md = MDNode::get(ctx, MDString::get(ctx, mkstr<err_t>()));
    inst->setMetadata(MKINT_IR_ERR, md);
}

template <interr err_t, typename I> static std::enable_if_t<!std::is_pointer_v<I>> mark_err(I& inst)
{
    mark_err<err_t>(&inst);
}

static void mark_taint(Instruction& inst, std::string_view taint_name = "")
{
    auto& ctx = inst.getContext();
    auto md = MDNode::get(ctx, MDString::get(ctx, taint_name));
    inst.setMetadata(MKINT_IR_TAINT, md);
}

static bool is_taint_src(StringRef sv)
{
    const auto demangled_name = demangle(sv.str().c_str());
    const auto name = StringRef(demangled_name);
    return name.startswith("sys_") || name.startswith("__mkint_ann_");
}

static std::vector<CallInst*> get_taint_source(Function& F)
{
    std::vector<CallInst*> ret;
    // judge if this function is the taint source.
    const auto name = F.getName();
    if (is_taint_src(name)) {
        // mark all this function as a taint source.
        // Unfortunately arguments cannot be marked with metadata...
        // We need to rewrite the arguments -> unary callers and mark the callers.
        for (auto& arg : F.args()) {
            auto itype = dyn_cast<IntegerType>(arg.getType());
            if (nullptr == itype || arg.use_empty()) {
                continue;
            }
            auto call_name = name.str() + MKINT_TAINT_SRC_SUFFX + std::to_string(arg.getArgNo());
            MKINT_LOG() << "Taint Analysis -> taint src arg -> call inst: " << call_name;
            auto call_inst = CallInst::Create(F.getParent()->getOrInsertFunction(call_name, itype), arg.getName(),
                &*F.getEntryBlock().getFirstInsertionPt());
            ret.push_back(call_inst);
            arg.replaceAllUsesWith(call_inst);
        }
    }
    return ret;
}

static bool is_taint_src_arg_call(StringRef s) { return s.contains(MKINT_TAINT_SRC_SUFFX); }

crange compute_binary_rng(const BinaryOperator* op, crange lhs, crange rhs)
{
    switch (op->getOpcode()) {
    case Instruction::Add:
        return lhs.add(rhs);
    case Instruction::Sub:
        return lhs.sub(rhs);
    case Instruction::Mul:
        return lhs.multiply(rhs);
    case Instruction::UDiv:
        return lhs.udiv(rhs);
    case Instruction::SDiv:
        return lhs.sdiv(rhs);
    case Instruction::Shl:
        return lhs.shl(rhs);
    case Instruction::LShr:
        return lhs.lshr(rhs);
    case Instruction::AShr:
        return lhs.ashr(rhs);
    case Instruction::And:
        return lhs.binaryAnd(rhs);
    case Instruction::Or:
        return lhs.binaryOr(rhs);
    case Instruction::Xor:
        return lhs.binaryXor(rhs);
    case Instruction::URem:
        return lhs.urem(rhs);
    case Instruction::SRem:
        return lhs.srem(rhs);
    default:
        MKINT_LOG() << "Unhandled binary opcode: " << op->getOpcodeName();
    }

    return rhs;
}

struct MKintPass : public PassInfoMixin<MKintPass> {
    MKintPass()
        : m_solver(std::nullopt)
    {
    }

    void backedge_analysis(const Function& F)
    {
        for (const auto& bb_ref : F) {
            auto bb = &bb_ref;
            if (m_backedges.count(bb) == 0) {
                // compute backedges of bb
                m_backedges[bb] = {};
                std::vector<const BasicBlock*> remote_succs { bb };
                while (!remote_succs.empty()) {
                    auto cur_succ = remote_succs.back();
                    remote_succs.pop_back();
                    for (const auto succ : successors(cur_succ)) {
                        if (succ != bb && !m_backedges[bb].contains(succ)) {
                            m_backedges[bb].insert(succ);
                            remote_succs.push_back(succ);
                        }
                    }
                }
            }
        }
    }

    crange get_range(const Value* var, DenseMap<const Value*, crange>& brange)
    {
        if (brange.count(var)) {
            return brange[var];
        }

        if (auto lconst = dyn_cast<ConstantInt>(var)) {
            return crange(lconst->getValue());
        } else {
            if (auto gv = dyn_cast<GlobalVariable>(var))
                return m_global2range[gv];
        }
        MKINT_WARN() << "Unknown operand type: " << *var;
        return crange(var->getType()->getIntegerBitWidth(), true);
    }

    crange get_range_by_bb(const Value* var, const BasicBlock* bb)
    {
        return get_range(var, m_func2range_info[bb->getParent()][bb]);
    }

    void analyze_one_bb_range(BasicBlock* bb, DenseMap<const Value*, crange>& cur_rng)
    {
        auto& F = *bb->getParent();
        auto& sum_rng = m_func2range_info[&F][bb];
        for (auto& inst : bb->getInstList()) {
            const auto get_rng = [&cur_rng, this](auto var) { return get_range(var, cur_rng); };
            // Store / Call / Return
            if (const auto call = dyn_cast<CallInst>(&inst)) {
                if (const auto f = call->getCalledFunction()) {
                    if (m_callback_tsrc_fn.contains(f->getName())) {
                        const auto& argcalls = m_func2tsrc[f];

                        for (const auto& arg : f->args()) {
                            auto& argblock = m_func2range_info[f][&(f->getEntryBlock())];
                            const size_t arg_idx = arg.getArgNo();
                            if (arg.getType()->isIntegerTy()) {
                                argblock[&arg] = get_rng(call->getArgOperand(arg_idx)).unionWith(argblock[&arg]);
                                m_func2ret_range[argcalls[arg_idx]->getCalledFunction()] = argblock[&arg];
                            }
                        }
                    } else {
                        for (const auto& arg : f->args()) {
                            auto& argblock = m_func2range_info[f][&(f->getEntryBlock())];
                            if (arg.getType()->isIntegerTy())
                                argblock[&arg] = get_rng(call->getArgOperand(arg.getArgNo())).unionWith(argblock[&arg]);
                        }
                    }

                    if (f->getReturnType()->isIntegerTy()) // return value is integer.
                        cur_rng[call] = m_func2ret_range[f];
                }

                continue;
            } else if (const auto store = dyn_cast<StoreInst>(&inst)) {
                // is global var
                const auto val = store->getValueOperand();
                const auto ptr = store->getPointerOperand();

                if (!val->getType()->isIntegerTy())
                    continue;

                auto valrng = get_rng(val);
                if (const auto gv = dyn_cast<GlobalVariable>(ptr)) {
                    // should be lazy mode. check local vars first and then check global vars.
                    m_global2range[gv] = m_global2range[gv].unionWith(valrng);
                } else if (const auto gep = dyn_cast<GetElementPtrInst>(ptr)) {
                    auto gep_addr = gep->getPointerOperand();
                    if (auto garr = dyn_cast<GlobalVariable>(gep_addr)) {
                        if (m_garr2ranges.count(garr) && gep->getNumIndices() == 2) { // all one dim array<int>s!
                            auto idx = gep->getOperand(2);
                            const size_t arr_size = m_garr2ranges[garr].size();
                            const crange idx_rng = get_rng(idx);
                            const size_t idx_max = idx_rng.getUnsignedMax().getLimitedValue();
                            if (idx_max >= arr_size)
                                m_gep_oob.insert(gep);

                            for (size_t i = idx_rng.getUnsignedMin().getLimitedValue(); i < std::min(arr_size, idx_max);
                                 ++i) {
                                m_garr2ranges[garr][i] = m_garr2ranges[garr][i].unionWith(valrng);
                            }
                        }
                    }
                }

                // is local var
                cur_rng[ptr] = valrng; // better precision.
                continue;
            } else if (const auto ret = dyn_cast<ReturnInst>(&inst)) {
                // low precision: just apply!
                if (F.getReturnType()->isIntegerTy())
                    m_func2ret_range[&F] = get_rng(ret->getReturnValue()).unionWith(m_func2ret_range[&F]);

                continue;
            }

            // return type should be int
            if (!inst.getType()->isIntegerTy()) {
                continue;
            }

            // empty range
            crange new_range = crange::getEmpty(inst.getType()->getIntegerBitWidth());

            if (const BinaryOperator* op = dyn_cast<BinaryOperator>(&inst)) {
                auto lhs = op->getOperand(0);
                auto rhs = op->getOperand(1);

                crange lhs_range = get_rng(lhs), rhs_range = get_rng(rhs);
                new_range = compute_binary_rng(op, lhs_range, rhs_range);
                // NOTE: LLVM is not a fan of unary operators.
                //       -x is represented by 0 - x...
            } else if (const SelectInst* op = dyn_cast<SelectInst>(&inst)) {
                const auto tval = op->getTrueValue();
                const auto fval = op->getFalseValue();
                new_range = get_rng(tval).unionWith(get_rng(fval));
            } else if (const CastInst* op = dyn_cast<CastInst>(&inst)) {
                new_range = [op, &get_rng]() -> crange {
                    auto inp = op->getOperand(0);
                    if (!inp->getType()->isIntegerTy())
                        return crange(op->getType()->getIntegerBitWidth(), true);
                    auto inprng = get_rng(inp);
                    const uint32_t bits = op->getType()->getIntegerBitWidth();
                    switch (op->getOpcode()) {
                    case CastInst::Trunc:
                        return inprng.truncate(bits);
                    case CastInst::ZExt:
                        return inprng.zeroExtend(bits);
                    case CastInst::SExt:
                        return inprng.signExtend(bits); // FIXME: Crash on M1 Mac?
                                                        // But it is not a problem on Linux.
                    default:
                        MKINT_LOG() << "Unhandled Cast Instruction " << op->getOpcodeName()
                                    << ". Using original range.";
                    }

                    return inprng;
                }();
            } else if (const PHINode* op = dyn_cast<PHINode>(&inst)) {
                for (size_t i = 0; i < op->getNumIncomingValues(); ++i) {
                    auto pred = op->getIncomingBlock(i);
                    if (m_backedges[bb].contains(pred)) {
                        continue; // skip backedge
                    }
                    new_range = new_range.unionWith(get_range_by_bb(op->getIncomingValue(i), pred));
                }
            } else if (auto op = dyn_cast<LoadInst>(&inst)) {
                auto addr = op->getPointerOperand();
                if (dyn_cast<GlobalVariable>(addr))
                    new_range = get_rng(addr);
                else if (auto gep = dyn_cast<GetElementPtrInst>(addr)) {
                    bool succ = false;
                    // we only analyze shallow arrays. i.e., one dim.
                    auto gep_addr = gep->getPointerOperand();
                    if (auto garr = dyn_cast<GlobalVariable>(gep_addr)) {
                        if (m_garr2ranges.count(garr) && gep->getNumIndices() == 2) { // all one dim array<int>s!
                            auto idx = gep->getOperand(2);
                            const size_t arr_size = m_garr2ranges[garr].size();
                            const crange idx_rng = get_rng(idx);
                            const size_t idx_max = idx_rng.getUnsignedMax().getLimitedValue();
                            if (idx_max >= arr_size) {
                                m_gep_oob.insert(gep);
                            }

                            for (size_t i = idx_rng.getUnsignedMin().getLimitedValue(); i < std::min(arr_size, idx_max);
                                 ++i) {
                                new_range = new_range.unionWith(m_garr2ranges[garr][i]);
                            }

                            succ = true;
                        }
                    }

                    if (!succ) {
                        MKINT_WARN() << "Unknown address to load (unknow gep src addr): " << inst;
                        new_range = crange(op->getType()->getIntegerBitWidth(), true); // unknown addr -> full range.
                    }
                } else {
                    MKINT_WARN() << "Unknown address to load: " << inst;
                    new_range = crange(op->getType()->getIntegerBitWidth()); // unknown addr -> full range.
                }
            } else if (const auto op = dyn_cast<CmpInst>(&inst)) {
                // can be more precise by comparing the range...
                // but nah...
            } else {
                MKINT_CHECK_RELAX(false) << " [Range Analysis] Unhandled instruction: " << inst;
            }

            cur_rng[&inst] = new_range.unionWith(cur_rng[&inst]);
        }

        if (&cur_rng != &sum_rng)
            for (auto [bb, rng] : cur_rng)
                sum_rng[bb] = sum_rng.count(bb) ? sum_rng[bb].unionWith(rng) : rng;
    }

    void range_analysis(Function& F)
    {
        MKINT_LOG() << "Range Analysis -> " << F.getName();

        auto& bb_range = m_func2range_info[&F];

        for (auto& bbref : F) {
            auto bb = &bbref;

            auto& sum_rng = bb_range[bb];

            // merge all incoming bbs
            for (const auto& pred : predecessors(bb)) {
                // avoid backedge: pred can't be a successor of bb.
                if (m_backedges[bb].contains(pred))
                    continue; // skip backedge

                MKINT_LOG() << "Merging: " << get_bb_label(pred) << "\t -> " << get_bb_label(bb);
                auto branch_rng = bb_range[pred];
                if (auto terminator = pred->getTerminator(); auto br = dyn_cast<BranchInst>(terminator)) {
                    if (br->isConditional()) {
                        if (auto cmp = dyn_cast<ICmpInst>(br->getCondition())) {
                            // br: a op b == true or false
                            // makeAllowedICmpRegion turning a op b into a range.
                            auto lhs = cmp->getOperand(0);
                            auto rhs = cmp->getOperand(1);

                            if (!lhs->getType()->isIntegerTy() || !rhs->getType()->isIntegerTy()) {
                                // This should be covered by `ICmpInst`.
                                MKINT_WARN() << "The br operands are not both integers: " << *cmp;
                            } else {
                                auto lrng = get_range_by_bb(lhs, pred), rrng = get_range_by_bb(rhs, pred);

                                bool is_true_br = br->getSuccessor(0) == bb;
                                if (is_true_br) { // T branch
                                    crange lprng = crange::cmpRegion()(cmp->getPredicate(), rrng);
                                    crange rprng = crange::cmpRegion()(cmp->getSwappedPredicate(), lrng);

                                    // Don't change constant's value.
                                    branch_rng[lhs] = dyn_cast<ConstantInt>(lhs) ? lrng : lrng.intersectWith(lprng);
                                    branch_rng[rhs] = dyn_cast<ConstantInt>(rhs) ? rrng : rrng.intersectWith(rprng);
                                } else { // F branch
                                    crange lprng = crange::cmpRegion()(cmp->getInversePredicate(), rrng);
                                    crange rprng
                                        = crange::cmpRegion()(CmpInst::getInversePredicate(cmp->getPredicate()), lrng);
                                    // Don't change constant's value.
                                    branch_rng[lhs] = dyn_cast<ConstantInt>(lhs) ? lrng : lrng.intersectWith(lprng);
                                    branch_rng[rhs] = dyn_cast<ConstantInt>(rhs) ? rrng : rrng.intersectWith(rprng);
                                }

                                if (branch_rng[lhs].isEmptySet() || branch_rng[rhs].isEmptySet())
                                    m_impossible_branches[cmp] = is_true_br; // TODO: higher precision.
                                else
                                    branch_rng[cmp] = crange(APInt(1, is_true_br));
                            }
                        }
                    }
                } else if (auto swt = dyn_cast<SwitchInst>(terminator)) {
                    auto cond = swt->getCondition();
                    if (cond->getType()->isIntegerTy()) {
                        auto cond_rng = get_range_by_bb(cond, pred);
                        auto emp_rng = crange::getEmpty(cond->getType()->getIntegerBitWidth());

                        if (swt->getDefaultDest() == bb) { // default
                            // not (all)
                            for (auto c : swt->cases()) {
                                auto case_val = c.getCaseValue();
                                emp_rng = emp_rng.unionWith(case_val->getValue());
                            }
                            emp_rng = emp_rng.inverse();
                        } else {
                            for (auto c : swt->cases()) {
                                if (c.getCaseSuccessor() == bb) {
                                    auto case_val = c.getCaseValue();
                                    emp_rng = emp_rng.unionWith(case_val->getValue());
                                }
                            }
                        }

                        branch_rng[cond] = cond_rng.unionWith(emp_rng);
                    }
                } else {
                    // try catch... (thank god, C does not have try-catch)
                    // indirectbr... ?
                    MKINT_CHECK_ABORT(false) << "Unknown terminator: " << *pred->getTerminator();
                }

                analyze_one_bb_range(bb, branch_rng);
            }

            if (bb->isEntryBlock()) {
                MKINT_LOG() << "No predecessors: " << bb;
                analyze_one_bb_range(bb, sum_rng);
            }
        }
    }

    static std::string get_bb_label(const BasicBlock* bb)
    {
        std::string str;
        raw_string_ostream os(str);
        bb->printAsOperand(os, false);

        return std::move(str);
    }

    static SmallVector<Function*, 2> get_sink_fns(Instruction* inst) noexcept
    {
        SmallVector<Function*, 2> ret;
        for (auto user : inst->users()) {
            if (auto call = dyn_cast<CallInst>(user)) {
                auto dname = demangle(call->getCalledFunction()->getName().data());
                if (std::find_if(
                        MKINT_SINKS.begin(), MKINT_SINKS.end(), [&dname](const auto& s) { return dname == s.first; })
                    != MKINT_SINKS.end()) {
                    ret.push_back(call->getCalledFunction());
                }
            }
        }
        return ret;
    }

    bool is_sink_reachable(Instruction* inst)
    {
        // we want to only mark sink-reachable taints; and
        // find out if the return value is tainted.
        if (nullptr == inst) {
            return false;
        } else if (inst->getMetadata(MKINT_IR_SINK)) {
            for (auto f : get_sink_fns(inst)) {
                m_taint_funcs.insert(f); // sink are tainted.
            }
            return true;
        }

        bool you_see_sink /* ? */ = false;

        // if store
        if (auto store = dyn_cast<StoreInst>(inst)) {
            auto ptr = store->getPointerOperand();
            if (auto gv = dyn_cast<GlobalVariable>(ptr)) {
                for (auto user : gv->users()) {
                    if (auto user_inst = dyn_cast<Instruction>(user)) {
                        if (user != store) // no self-loop.
                            you_see_sink |= is_sink_reachable(user_inst);
                    }
                }

                if (you_see_sink) {
                    mark_taint(*inst);
                    gv->setMetadata(MKINT_IR_TAINT, inst->getMetadata(MKINT_IR_TAINT));
                    return true;
                }
            }
        } else {
            if (auto call = dyn_cast<CallInst>(inst)) {
                if (auto f = call->getCalledFunction()) {
                    // How to do taint analysis for call func?
                    // if func's impl is unknow we simply assume it is related.
                    // if func's impl is known, we analyze which arg determines the return value.
                    // if unrelated -> cut off the connection.
                    // FIXME: But we simply assume it is related and people won't wrote stupid code that results are not
                    // related to inputs.
                    if (!f->isDeclaration() && taint_bcast_sink(f->args())) {
                        you_see_sink = true;
                        m_taint_funcs.insert(f);
                    }
                }
            }

            for (auto user : inst->users()) {
                if (auto user_inst = dyn_cast<Instruction>(user)) {
                    // if used by phi whose id is even smaller than you. -> loop
                    if (auto phi = dyn_cast<PHINode>(user_inst)) {
                        if (get_id(phi) < get_id(inst)) {
                            continue;
                        }
                    }
                    you_see_sink |= is_sink_reachable(user_inst);
                }
            }

            if (you_see_sink) {
                mark_taint(*inst);
                if (auto call = dyn_cast<CallInst>(inst)) {
                    if (auto f = call->getCalledFunction()) {
                        if (!f->getReturnType()->isVoidTy()) {
                            m_taint_funcs.insert(f);
                        }
                    }
                }
                return true;
            }
        }

        return false;
    }

    bool taint_bcast_sink(const std::vector<CallInst*>& taint_source)
    {
        // ? Note we currently assume that sub-func-calls do not have sinks...
        // ? otherwise we need a use-def tree to do the job (but too complicated).
        // Propogation: This pass should only consider single-function-level tainting.
        //              In `out = call(..., taint, ...)`, `out` is tainted. But let's
        //              refine that in cross-function-level tainting.

        // Algo: should do depth-first search until we find a sink. If we find a sink,
        //       we backtrack and mark taints.

        bool ret = false;

        for (auto ts : taint_source) {
            if (is_sink_reachable(ts)) {
                mark_taint(*ts, "source");
                ret = true;
            }
        }

        return ret;
    }

    template <typename Iter> bool taint_bcast_sink(Iter taint_source)
    {
        bool ret = false;

        for (auto& ts : taint_source) {
            for (auto user : ts.users()) {
                if (auto user_inst = dyn_cast<Instruction>(user)) {
                    if (is_sink_reachable(user_inst)) {
                        mark_taint(*user_inst);
                        ret = true;
                    }
                }
            }
        }

        return ret;
    }

    void mark_func_sinks(Function& F)
    {
        static auto mark_sink = [](Instruction& inst, std::string_view sink_name) {
            auto& ctx = inst.getContext();
            auto md = MDNode::get(ctx, MDString::get(ctx, sink_name));
            inst.setMetadata(MKINT_IR_SINK, md);
        };

        for (auto& inst : instructions(F)) {
            if (auto* call = dyn_cast<CallInst>(&inst)) {
                // call in MKINT_SINKS
                for (const auto& [name, idx] : MKINT_SINKS) {
                    if (auto called_fn = call->getCalledFunction()) {
                        const auto demangled_func_name = demangle(called_fn->getName().str().c_str());
                        if (demangled_func_name == name) {
                            if (auto arg = dyn_cast_or_null<Instruction>(call->getArgOperand(idx))) {
                                MKINT_LOG()
                                    << "Taint Analysis -> sink: argument [" << idx << "] of " << demangled_func_name;
                                mark_sink(*arg, name);
                            }
                            break;
                        } else if (StringRef(demangled_func_name).startswith(name)) {
                            MKINT_WARN() << "Are you missing the sink? [demangled_func_name]: " << demangled_func_name
                                         << "; [name]: " << name;
                        }
                    }
                }
            }
        }

        // if this function is taint source and has a return value used non-taint-source functions, we mark its return
        // statement as sink. this is because its return value can be used by, say kernel functions.
        if (is_taint_src(F.getName()) && F.getReturnType()->isIntegerTy() && !F.use_empty()) {

            // if there is any users.
            bool valid_use = false;
            for (auto user : F.users()) {
                if (auto user_inst = dyn_cast<Instruction>(user)) {
                    if (!is_taint_src(user_inst->getParent()->getParent()->getName())) {
                        valid_use = true;
                        break;
                    }
                }
            }
            if (!valid_use)
                return;

            for (auto& inst : instructions(F)) {
                if (dyn_cast<ReturnInst>(&inst)) {
                    MKINT_LOG() << "Taint Analysis -> sink: return inst of " << F.getName();
                    mark_sink(inst, "return");
                    m_callback_tsrc_fn.insert(F.getName());
                }
            }
        }
    }

    PreservedAnalyses run(Module& M, ModuleAnalysisManager& MAM)
    {
        MKINT_LOG() << "Running MKint pass on module " << M.getName();

        // FIXME: This is a hack.
        auto ctx = new z3::context; // let it leak.
        m_solver = z3::solver(*ctx);

        // Mark taint sources.
        for (auto& F : M) {
            auto taint_sources = get_taint_source(F);
            mark_func_sinks(F);
            if (is_taint_src(F.getName()))
                m_func2tsrc[&F] = std::move(taint_sources);
        }

        for (auto [fp, tsrc] : m_func2tsrc) {
            if (taint_bcast_sink(tsrc)) {
                m_taint_funcs.insert(fp);
            }
        }

        size_t n_tfunc_before = 0;
        do {
            n_tfunc_before = m_taint_funcs.size();
            for (auto f : m_taint_funcs) {
                if (!is_taint_src(f->getName())) {
                    taint_bcast_sink(f->args());
                }
            }
        } while (n_tfunc_before != m_taint_funcs.size());

        constexpr size_t max_try = 128;
        size_t try_count = 0;

        for (auto& F : M) {
            if (!F.isDeclaration()) {
                backedge_analysis(F);
            }
        }

        MKINT_LOG() << "Module after taint:";
        MKINT_LOG() << M;

        this->init_ranges(M);
        while (true) { // iterative range analysis.
            const auto old_fn_rng = m_func2range_info;
            const auto old_glb_rng = m_global2range;
            const auto old_glb_arrrng = m_garr2ranges;
            const auto old_fn_ret_rng = m_func2ret_range;

            for (auto F : m_range_analysis_funcs) {
                range_analysis(*F);
            }

            if (m_func2range_info == old_fn_rng && old_glb_rng == m_global2range && old_fn_ret_rng == m_func2ret_range
                && old_glb_arrrng == m_garr2ranges)
                break;
            if (++try_count > max_try) {
                MKINT_LOG() << "[Iterative Range Analysis] "
                            << "Max try " << max_try << " reached, aborting.";
                break;
            }
        }
        this->pring_all_ranges();

        this->smt_solving(M);

        this->mark_errors();

        return PreservedAnalyses::all();
    }

    void init_ranges(Module& M)
    {
        for (auto& F : M) {
            // Functions for range analysis:
            // 1. taint source -> taint sink.
            // 2. integer functions.
            MKINT_LOG() << "Init Range Analysis: " << F.getName();
            if (F.getReturnType()->isIntegerTy() || m_taint_funcs.contains(&F)) {
                if (F.isDeclaration()) {
                    if (is_taint_src_arg_call(F.getName()) && !m_taint_funcs.contains(&F) // will not call sink fns.
                        && m_callback_tsrc_fn.contains(
                            F.getName().substr(0, F.getName().size() - StringRef(MKINT_TAINT_SRC_SUFFX).size() - 1))) {
                        if (F.getReturnType()->isIntegerTy())
                            m_func2ret_range[&F] = crange(F.getReturnType()->getIntegerBitWidth(), false);
                        MKINT_LOG() << "Skip range analysis for func w/o impl [Empty Set]: " << F.getName()
                                    << "\tin taint_funcs? ";
                    } else {
                        if (F.getReturnType()->isIntegerTy())
                            m_func2ret_range[&F] = crange(F.getReturnType()->getIntegerBitWidth(), true); // full.
                        MKINT_LOG() << "Skip range analysis for func w/o impl [Full Set]: " << F.getName();
                    }
                } else {
                    if (F.getReturnType()->isIntegerTy())
                        m_func2ret_range[&F] = crange(F.getReturnType()->getIntegerBitWidth(), false); // empty.

                    // init the arg range
                    auto& init_blk = m_func2range_info[&F][&(F.getEntryBlock())];
                    for (const auto& arg : F.args()) {
                        if (arg.getType()->isIntegerTy()) {
                            // be conservative first.
                            // TODO: fine-grained arg range (some taint, some not)
                            if (is_taint_src(F.getName())
                                && !m_callback_tsrc_fn.contains(F.getName())) { // for taint source, we assume full set.
                                init_blk[&arg] = crange(arg.getType()->getIntegerBitWidth(), true);
                            } else {
                                init_blk[&arg] = crange(arg.getType()->getIntegerBitWidth(), false);
                            }
                        }
                    }
                    m_range_analysis_funcs.insert(&F);
                }
            }
        }

        // m_callback_tsrc_fn's highest user's input is set as full set.
        for (auto& fn : m_callback_tsrc_fn) {
            auto cbf = M.getFunction(fn);

            std::deque<Function*> worklist;
            SetVector<Function*> hist;

            worklist.push_back(cbf);
            hist.insert(cbf);

            while (!worklist.empty()) {
                auto cur = worklist.front();
                worklist.pop_front();

                if (cur->user_empty()) {
                    for (const auto& arg : cur->args()) {
                        m_func2range_info[cur][&(cur->getEntryBlock())][&arg]
                            = crange(arg.getType()->getIntegerBitWidth(), true);
                        ;
                    }
                } else {
                    for (const auto& u : cur->users()) {
                        if (auto uu = dyn_cast<CallInst>(u)) {
                            auto caller = uu->getCalledFunction();
                            if (!hist.contains(caller)) {
                                worklist.push_back(caller);
                                hist.insert(caller);
                            }
                        }
                    }
                }
            }
        }

        // global variables
        for (const auto& GV : M.globals()) {
            MKINT_LOG() << "Found global var " << GV.getName() << " of type " << *GV.getType();
            // TODO: handle struct (ptr); array (ptr)
            if (GV.getValueType()->isIntegerTy()) {
                if (GV.hasInitializer()) {
                    auto init_val = dyn_cast<ConstantInt>(GV.getInitializer())->getValue();
                    MKINT_LOG() << GV.getName() << " init by " << init_val;
                    m_global2range[&GV] = crange(init_val);
                } else {
                    m_global2range[&GV] = crange(GV.getType()->getIntegerBitWidth()); // can be all range.
                }
            } else if (GV.getValueType()->isArrayTy()) { // int array.
                const auto garr = dyn_cast<ArrayType>(GV.getValueType());
                if (garr->getElementType()->isIntegerTy()) {
                    if (GV.hasInitializer()) {
                        if (auto darr = dyn_cast<ConstantDataArray>(GV.getInitializer())) {
                            for (size_t i = 0; i < darr->getNumElements(); i++) {
                                auto init_val = dyn_cast<ConstantInt>(darr->getElementAsConstant(i))->getValue();
                                MKINT_LOG() << GV.getName() << "[" << i << "] init by " << init_val;
                                m_garr2ranges[&GV].push_back(crange(init_val));
                            }
                        } else if (auto zinit = dyn_cast<ConstantAggregateZero>(GV.getInitializer())) {
                            for (size_t i = 0; i < zinit->getElementCount().getFixedValue(); i++)
                                m_garr2ranges[&GV].push_back(crange(
                                    APInt::getNullValue(zinit->getElementValue(i)->getType()->getIntegerBitWidth())));
                        } else {
                            MKINT_CHECK_ABORT(false) << "Unsupported initializer for global array: " << GV.getName();
                        }
                    } else {
                        for (size_t i = 0; i < garr->getNumElements(); i++) {
                            m_garr2ranges[&GV].push_back( // can be anything
                                crange(garr->getElementType()->getIntegerBitWidth(), true));
                        }
                    }
                } else {
                    MKINT_WARN() << "Unhandled global var type: " << *GV.getType() << " -> " << GV.getName();
                }
            } else {
                MKINT_WARN() << "Unhandled global var type: " << *GV.getType() << " -> " << GV.getName();
            }
        }
    }

    void pring_all_ranges() const
    {
        MKINT_LOG() << "========== Function Return Ranges ==========";
        for (const auto& [F, rng] : m_func2ret_range) {
            MKINT_LOG() << rang::bg::black << rang::fg::green << F->getName() << rang::style::reset << " -> " << rng;
        }

        MKINT_LOG() << "========== Global Variable Ranges ==========";
        for (const auto& [GV, rng] : m_global2range) {
            MKINT_LOG() << rang::bg::black << rang::fg::blue << GV->getName() << rang::style::reset << " -> " << rng;
        }

        for (const auto& [GV, rng_vec] : m_garr2ranges) {
            for (size_t i = 0; i < rng_vec.size(); i++) {
                MKINT_LOG() << rang::bg::black << rang::fg::blue << GV->getName() << "[" << i << "]"
                            << rang::style::reset << " -> " << rng_vec[i];
            }
        }

        MKINT_LOG() << "============ Function Inst Ranges ============";
        for (const auto& [F, blk2rng] : m_func2range_info) {
            MKINT_LOG() << " ----------- Function Name : " << rang::bg::black << rang::fg::green << F->getName()
                        << rang::style::reset;
            for (const auto& [blk, inst2rng] : blk2rng) {
                MKINT_LOG() << " ----------- Basic Block ----------- ";
                for (const auto& [val, rng] : inst2rng) {
                    if (dyn_cast<ConstantInt>(val))
                        continue; // meaningless to pring const range.

                    if (rng.isFullSet())
                        MKINT_LOG() << *val << "\t -> " << rng;
                    else
                        MKINT_LOG() << *val << "\t -> " << rang::bg::black << rang::fg::yellow << rng
                                    << rang::style::reset;
                }
            }
        }

        if (!m_impossible_branches.empty())
            MKINT_LOG() << "============" << rang::fg::yellow << rang::style::bold << " Impossible Branches "
                        << rang::style::reset << "============";
        for (auto [cmp, is_tbr] : m_impossible_branches) {
            MKINT_WARN() << rang::bg::black << rang::fg::red << cmp->getFunction()->getName() << "::" << *cmp
                         << rang::style::reset << "'s " << rang::fg::red << rang::style::italic
                         << (is_tbr ? "true" : "false") << rang::style::reset << " branch";
        }

        if (!m_gep_oob.empty())
            MKINT_LOG() << "============" << rang::fg::yellow << rang::style::bold << " Array Index Out of Bound "
                        << rang::style::reset << "============";
        for (auto gep : m_gep_oob) {
            MKINT_WARN() << rang::bg::black << rang::fg::red << gep->getFunction()->getName() << "::" << *gep
                         << rang::style::reset;
        }
    }

    bool add_range_cons(const crange rng, const z3::expr& bv)
    {
        if (rng.isFullSet() || bv.is_const())
            return true;

        if (rng.isEmptySet()) {
            MKINT_CHECK_RELAX(false) << "lhs is empty set";
            return false;
        }

        m_solver.value().add(
            z3::ule(bv, m_solver.value().ctx().bv_val(rng.getUnsignedMax().getZExtValue(), rng.getBitWidth())));
        m_solver.value().add(
            z3::uge(bv, m_solver.value().ctx().bv_val(rng.getUnsignedMin().getZExtValue(), rng.getBitWidth())));
        return true;
    }

    // for general: check overflow;
    // for shl:     check shift amount;
    // for div:     check divisor != 0;
    void binary_check(BinaryOperator* op)
    {
        const auto& lhs_bv = v2sym(op->getOperand(0));
        const auto& rhs_bv = v2sym(op->getOperand(1));
        const auto rhs_bits = rhs_bv.get_sort().bv_size();

        const auto [is_nsw, is_nuw] = [op] {
            if (const auto ofop = dyn_cast<OverflowingBinaryOperator>(op)) {
                return std::make_pair(ofop->hasNoSignedWrap(), ofop->hasNoUnsignedWrap());
            }
            return std::make_pair(false, false);
        }();

        const auto check = [&, this](interr et, bool is_signed) {
            if (m_solver.value().check() == z3::sat) { // counter example
                z3::model m = m_solver.value().get_model();
                MKINT_WARN() << rang::fg::yellow << rang::style::bold << mkstr(et) << rang::style::reset << " at "
                             << rang::bg::black << rang::fg::red << op->getParent()->getParent()->getName()
                             << "::" << *op << rang::style::reset;
                auto lhs_bin = m.eval(lhs_bv, true);
                auto rhs_bin = m.eval(rhs_bv, true);
                if (is_signed) {
                    MKINT_WARN() << "Counter example: " << rang::bg::black << rang::fg::red << op->getOpcodeName()
                                 << '(' << lhs_bin << ", " << rhs_bin << ") -> " << op->getOpcodeName() << '('
                                 << lhs_bin.as_int64() << ", " << rhs_bin.as_int64() << ')' << rang::style::reset;
                } else {
                    MKINT_WARN() << "Counter example: " << rang::bg::black << rang::fg::red << op->getOpcodeName()
                                 << '(' << lhs_bin << ", " << rhs_bin << ") -> " << op->getOpcodeName() << '('
                                 << lhs_bin.as_uint64() << ", " << rhs_bin.as_uint64() << ')' << rang::style::reset;
                }

                switch (et) {
                case interr::OVERFLOW:
                    m_overflow_insts.insert(op);
                    break;
                case interr::BAD_SHIFT:
                    m_bad_shift_insts.insert(op);
                    break;
                case interr::DIV_BY_ZERO:
                    m_div_zero_insts.insert(op);
                    break;
                default:
                    break;
                }
            }
        };

        m_solver.value().push();
        switch (op->getOpcode()) {
        case Instruction::Add:
            if (!is_nsw) { // unsigned
                m_solver.value().add(!z3 ::bvadd_no_overflow(lhs_bv, rhs_bv, false));
                check(interr::OVERFLOW, false);
            } else {
                m_solver.value().add(!z3::bvadd_no_overflow(lhs_bv, rhs_bv, true));
                m_solver.value().add(!z3::bvadd_no_underflow(lhs_bv, rhs_bv));
                check(interr::OVERFLOW, true);
            }

            break;
        case Instruction::Sub:
            if (!is_nsw) {
                m_solver.value().add(!z3::bvsub_no_underflow(lhs_bv, rhs_bv, false));
                check(interr::OVERFLOW, false);
            } else {
                m_solver.value().add(!z3::bvsub_no_underflow(lhs_bv, rhs_bv, true));
                m_solver.value().add(!z3::bvsub_no_overflow(lhs_bv, rhs_bv));
                check(interr::OVERFLOW, true);
            }

            break;
        case Instruction::Mul:
            if (!is_nsw) {
                m_solver.value().add(!z3::bvmul_no_overflow(lhs_bv, rhs_bv, false));
                check(interr::OVERFLOW, false);
            } else {
                m_solver.value().add(!z3::bvmul_no_overflow(lhs_bv, rhs_bv, true));
                m_solver.value().add(!z3::bvmul_no_underflow(lhs_bv, rhs_bv)); // INTMAX * -1
                check(interr::OVERFLOW, true);
            }
            break;
        case Instruction::URem:
        case Instruction::UDiv:
            m_solver.value().add(rhs_bv == m_solver.value().ctx().bv_val(0, rhs_bits));
            check(interr::DIV_BY_ZERO, false);
            break;
        case Instruction::SRem:
        case Instruction::SDiv: // can be overflow or divisor == 0
            m_solver.value().push();
            m_solver.value().add(rhs_bv == m_solver.value().ctx().bv_val(0, rhs_bits)); // may 0?
            check(interr::DIV_BY_ZERO, true);
            m_solver.value().pop();
            m_solver.value().add(z3::bvsdiv_no_overflow(lhs_bv, rhs_bv));
            check(interr::OVERFLOW, true);
            break;
        case Instruction::Shl:
        case Instruction::LShr:
        case Instruction::AShr:
            m_solver.value().add(rhs_bv >= m_solver.value().ctx().bv_val(rhs_bits, rhs_bits)); // sat means bug
            check(interr::BAD_SHIFT, false);
            break;
        case Instruction::And:
        case Instruction::Or:
        case Instruction::Xor:
            break;
        default:
            break;
        }
        m_solver.value().pop();
    }

    z3::expr binary_op_propagate(BinaryOperator* op)
    {
        const auto lhs = v2sym(op->getOperand(0));
        const auto rhs = v2sym(op->getOperand(1));
        switch (op->getOpcode()) {
        case Instruction::Add:
            return lhs + rhs;
        case Instruction::Sub:
            return lhs - rhs;
        case Instruction::Mul:
            return lhs * rhs;
        case Instruction::URem:
            return z3::urem(lhs, rhs);
        case Instruction::UDiv:
            return z3::udiv(lhs, rhs);
        case Instruction::SRem:
            return z3::srem(lhs, rhs);
        case Instruction::SDiv: // can be overflow or divisor == 0
            return lhs / rhs;
        case Instruction::Shl:
            return z3::shl(lhs, rhs);
        case Instruction::LShr:
            return z3::lshr(lhs, rhs);
        case Instruction::AShr:
            return z3::ashr(lhs, rhs);
        case Instruction::And:
            return lhs & rhs;
        case Instruction::Or:
            return lhs | rhs;
        case Instruction::Xor:
            return lhs ^ rhs;
        default:
            break;
        }

        MKINT_CHECK_ABORT(false) << "unsupported binary op: " << *op;
        return lhs; // dummy
    }

    z3::expr cast_op_propagate(CastInst* op)
    {
        const auto src = v2sym(op->getOperand(0));
        const uint32_t bits = op->getType()->getIntegerBitWidth();
        switch (op->getOpcode()) {
        case CastInst::Trunc:
            return src.extract(bits - 1, 0);
        case CastInst::ZExt:
            return z3::zext(src, bits - op->getOperand(0)->getType()->getIntegerBitWidth());
        case CastInst::SExt:
            return z3::sext(src, bits - op->getOperand(0)->getType()->getIntegerBitWidth());
        default:
            MKINT_WARN() << "Unhandled Cast Instruction " << op->getOpcodeName() << ". Using original range.";
        }

        const std::string new_sym_str = "\%cast" + std::to_string(op->getValueID());
        return m_solver.value().ctx().bv_const(new_sym_str.c_str(), bits); // new expr
    }

    void mark_errors()
    {
        for (auto [cmp, is_tbr] : m_impossible_branches) {
            if (is_tbr)
                mark_err<interr::DEAD_TRUE_BR>(cmp);
            else
                mark_err<interr::DEAD_FALSE_BR>(cmp);
        }

        for (auto gep : m_gep_oob) {
            mark_err<interr::ARRAY_OOB>(gep);
        }

        for (auto inst : m_overflow_insts) {
            mark_err<interr::OVERFLOW>(inst);
        }

        for (auto inst : m_bad_shift_insts) {
            mark_err<interr::BAD_SHIFT>(inst);
        }

        for (auto inst : m_div_zero_insts) {
            mark_err<interr::DIV_BY_ZERO>(inst);
        }
    }

    z3::expr v2sym(const Value* v)
    {
        if (auto it = m_v2sym.find(v); it != m_v2sym.end())
            return it->second.value();

        auto lconst = dyn_cast<ConstantInt>(v);
        MKINT_CHECK_ABORT(nullptr != lconst) << "unsupported value -> symbol mapping: " << *v;
        return m_solver.value().ctx().bv_val(lconst->getZExtValue(), lconst->getType()->getIntegerBitWidth());
    }

    void smt_solving(Module& M)
    {
        for (auto F : m_taint_funcs) {
            if (F->isDeclaration())
                continue;

            // Get a path tree.
            for (auto& bb : F->getBasicBlockList()) {
                for (const auto& pred : predecessors(&bb)) {
                    if (m_backedges[&bb].contains(pred) || &bb == pred)
                        continue;

                    m_bbpaths[pred].push_back(&bb);
                }
            }
        }

        // solve constraints.
        for (auto F : m_taint_funcs) {
            if (F->isDeclaration())
                continue;

            m_solver.value().push();
            // add function arg constraints.
            for (auto& arg : F->args()) {
                if (!arg.getType()->isIntegerTy())
                    continue;
                const auto arg_name = F->getName() + "." + std::to_string(arg.getArgNo());
                const auto argv
                    = m_solver.value().ctx().bv_const(arg_name.str().c_str(), arg.getType()->getIntegerBitWidth());
                m_v2sym[&arg] = argv;
                add_range_cons(get_range_by_bb(&arg, &(F->getEntryBlock())), argv);
            }

            path_solving(&(F->getEntryBlock()), nullptr);
            m_solver.value().pop();
        }
    }

    void path_solving(BasicBlock* cur, BasicBlock* pred)
    {
        if (m_backedges[cur].contains(pred))
            return;

        auto cur_brng = m_func2range_info[cur->getParent()][cur];

        if (nullptr != pred) {
            if (auto terminator = pred->getTerminator(); auto br = dyn_cast<BranchInst>(terminator)) {
                if (br->isConditional()) {
                    if (auto cmp = dyn_cast<ICmpInst>(br->getCondition())) {
                        // br: a op b == true or false
                        // makeAllowedICmpRegion turning a op b into a range.
                        auto lhs = cmp->getOperand(0);
                        auto rhs = cmp->getOperand(1);

                        if (!lhs->getType()->isIntegerTy() || !rhs->getType()->isIntegerTy()) {
                            // This should be covered by `ICmpInst`.
                            MKINT_WARN() << "The br operands are not both integers: " << *cmp;
                        } else {
                            bool is_true_br = br->getSuccessor(0) == cur;

                            if (m_impossible_branches.count(cmp) && m_impossible_branches[cmp] == is_true_br) {
                                return;
                            }

                            const auto get_tbr_assert = [lhs, rhs, cmp, this]() {
                                switch (cmp->getPredicate()) {
                                case ICmpInst::ICMP_EQ: // =
                                    return v2sym(lhs) == v2sym(rhs);
                                case ICmpInst::ICMP_NE: // !=
                                    return v2sym(lhs) != v2sym(rhs);
                                case ICmpInst::ICMP_SGT: // singed >
                                    return z3::sgt(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_SGE: // singed >=
                                    return z3::sge(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_SLT: // singed <
                                    return z3::slt(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_SLE: // singed <=
                                    return z3::sle(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_UGT: // unsigned >
                                    return z3::ugt(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_UGE: // unsigned >=
                                    return z3::uge(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_ULT: // unsigned <
                                    return z3::ult(v2sym(lhs), v2sym(rhs));
                                case ICmpInst::ICMP_ULE: // unsigned <=
                                    return z3::ule(v2sym(lhs), v2sym(rhs));
                                default:
                                    break;
                                }

                                MKINT_CHECK_ABORT(false) << "unsupported icmp predicate: " << *cmp;
                            };

                            const auto check = [cmp, is_true_br, this] {
                                if (m_solver.value().check() == z3::unsat) { // counter example
                                    MKINT_WARN() << "[SMT Solving] cannot continue " << (is_true_br ? "true" : "false")
                                                 << " branch of " << *cmp;
                                    return false;
                                }
                                return true;
                            };

                            if (is_true_br) { // T branch
                                m_solver.value().add(get_tbr_assert());
                                if (!check())
                                    return;
                                m_v2sym[cmp] = m_solver.value().ctx().bv_val(true, 1);
                            } else { // F branch
                                m_solver.value().add(!get_tbr_assert());
                                if (!check())
                                    return;
                                m_v2sym[cmp] = m_solver.value().ctx().bv_val(false, 1);
                            }
                        }
                    }
                }
            } else if (auto swt = dyn_cast<SwitchInst>(terminator)) {
                auto cond = swt->getCondition();
                if (cond->getType()->isIntegerTy()) {
                    auto cond_rng = get_range_by_bb(cond, pred);
                    auto emp_rng = crange::getEmpty(cond->getType()->getIntegerBitWidth());

                    if (swt->getDefaultDest() == cur) { // default
                        // not (all)
                        for (auto c : swt->cases()) {
                            auto case_val = c.getCaseValue();
                            m_solver.value().add(v2sym(cond)
                                != m_solver.value().ctx().bv_val(
                                    case_val->getZExtValue(), cond->getType()->getIntegerBitWidth()));
                        }
                    } else {
                        for (auto c : swt->cases()) {
                            if (c.getCaseSuccessor() == cur) {
                                auto case_val = c.getCaseValue();
                                m_solver.value().add(v2sym(cond)
                                    == m_solver.value().ctx().bv_val(
                                        case_val->getZExtValue(), cond->getType()->getIntegerBitWidth()));
                                break;
                            }
                        }
                    }
                }
            } else {
                // try catch... (thank god, C does not have try-catch)
                // indirectbr... ?
                MKINT_CHECK_ABORT(false) << "Unknown terminator: " << *pred->getTerminator();
            }
        }

        for (auto& inst : cur->getInstList()) {
            if (!cur_brng.count(&inst))
                continue;

            if (auto op = dyn_cast<BinaryOperator>(&inst)) {
                binary_check(op);
                m_v2sym[op] = binary_op_propagate(op);
                if (!add_range_cons(get_range_by_bb(&inst, inst.getParent()), v2sym(op)))
                    return;
            } else if (auto op = dyn_cast<CastInst>(&inst)) {
                m_v2sym[op] = cast_op_propagate(op);
                if (!add_range_cons(get_range_by_bb(&inst, inst.getParent()), v2sym(op)))
                    return;
            } else {
                const auto name = "\%vid" + std::to_string(inst.getValueID());
                m_v2sym[&inst] = m_solver.value().ctx().bv_const(name.c_str(), inst.getType()->getIntegerBitWidth());
                if (!add_range_cons(get_range_by_bb(&inst, inst.getParent()), v2sym(&inst)))
                    return;
            }
        }

        for (auto succ : m_bbpaths[cur]) {
            m_solver.value().push();
            path_solving(succ, cur);
            m_solver.value().pop();
        }
    }

private:
    MapVector<Function*, std::vector<CallInst*>> m_func2tsrc;
    SetVector<Function*> m_taint_funcs;
    DenseMap<const BasicBlock*, SetVector<const BasicBlock*>> m_backedges;
    SetVector<StringRef> m_callback_tsrc_fn;

    // for range analysis
    std::map<const Function*, bbrange_t> m_func2range_info;
    std::map<const Function*, crange> m_func2ret_range;
    SetVector<Function*> m_range_analysis_funcs;
    std::map<const GlobalVariable*, crange> m_global2range;
    std::map<const GlobalVariable*, SmallVector<crange, 4>> m_garr2ranges;

    // for error checking
    std::map<ICmpInst*, bool> m_impossible_branches;
    std::set<GetElementPtrInst*> m_gep_oob;
    std::set<Instruction*> m_overflow_insts;
    std::set<Instruction*> m_bad_shift_insts;
    std::set<Instruction*> m_div_zero_insts;

    // constraint solving
    std::optional<z3::solver> m_solver;
    DenseMap<const Value*, std::optional<z3::expr>> m_v2sym;
    std::map<const BasicBlock*, SmallVector<BasicBlock*, 2>> m_bbpaths;
};
} // namespace

// registering pass (new pass manager).
extern "C" ::llvm::PassPluginLibraryInfo LLVM_ATTRIBUTE_WEAK llvmGetPassPluginInfo()
{
    return { LLVM_PLUGIN_API_VERSION, "MKintPass", "v0.1", [](PassBuilder& PB) {
                PB.registerPipelineParsingCallback(
                    [](StringRef Name, ModulePassManager& MPM, ArrayRef<PassBuilder::PipelineElement>) {
                        if (Name == "mkint-pass") {
                            // do mem2reg.
                            MPM.addPass(createModuleToFunctionPassAdaptor(PromotePass()));
                            MPM.addPass(createModuleToFunctionPassAdaptor(SROAPass()));
                            MPM.addPass(MKintPass());
                            return true;
                        }
                        return false;
                    });
            } };
}

// the version number must match!
// get llvm ir: clang -Os -S -emit-llvm a.c
// or         : clang -O0 -Xclang -disable-O0-optnone -emit-llvm -S a.cpp
// test: opt -load-pass-plugin mkint/MiniKintPass.so -passes=mkint-pass -S a.ll
