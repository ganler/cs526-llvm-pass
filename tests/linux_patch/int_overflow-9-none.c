// http://git.kernel.org/linus/a1f74ae82d133ebb2aabb19d181944b4e83e9960
// mpt2sas-2011-1494

// RUN: clang-14 -D __PATCH__ -O0 -Xclang -disable-O0-optnone -emit-llvm -S %s -o %t.ll
// RUN: opt-14 -load-pass-plugin=%builddir/mkint/MiniKintPass.so -passes=mkint-pass -S %t.ll -o %t.out.ll

// RUN: BEFORE=%t.ll AFTER=%t.out.ll python3 %testdir/llvm_lite.py TestMKint.test_IR_correct
// RUN: BEFORE=%t.ll AFTER=%t.out.ll python3 %testdir/llvm_lite.py TestMKint.test_i_annoted


#include "linux.h"

struct MPT2SAS_ADAPTER {
	u16 request_sz;
};

struct mpt2_ioctl_command {
	uint32_t data_sge_offset;
};

enum block_state { NON_BLOCKING, BLOCKING };

long sys__ctl_do_mpt_command(struct MPT2SAS_ADAPTER *ioc,
			 struct mpt2_ioctl_command karg, void __user *mf, enum block_state state)
{
	void *mpi_request = NULL;
	long ret = 0;

	mpi_request = kzalloc(ioc->request_sz, GFP_KERNEL);
	if (!mpi_request) {
		ret = -ENOMEM;
		goto out;
	}
#ifdef __PATCH__
	/* Check for overflow and wraparound */
	if (karg.data_sge_offset * 4 > ioc->request_sz ||
	    karg.data_sge_offset > (UINT_MAX / 4)) {
		ret = -EINVAL;
		goto out;
	}
#endif
	/* copy in request message frame from user */
	if (copy_from_user(mpi_request, mf, karg.data_sge_offset*4)) { \
		// exp32: {{umul}}{{size}} \
		// exp64: {{umul}}
		ret = -EFAULT;
		goto out;
	}
out:
	kfree(mpi_request);
	return ret;
}
