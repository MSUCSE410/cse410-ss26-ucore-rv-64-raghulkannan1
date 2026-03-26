#include "syscall.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "proc.h"

uint64 sys_write(int fd, uint64 va, uint len)
{
	debugf("sys_write fd = %d va = %x, len = %d", fd, va, len);
	if (fd != STDOUT)
		return -1;
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	debugf("size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return size;
}

__attribute__((noreturn)) void sys_exit(int code)
{
	exit(code);
	__builtin_unreachable();
}

uint64 sys_sched_yield()
{
	yield();
	return 0;
}

uint64 sys_gettimeofday(TimeVal *val, int _tz)
{
	if (val == 0)
		return -1;

	struct proc *p = curr_proc();
	uint64 kva = useraddr(p->pagetable, (uint64)val);
	if (kva == 0)
		return -1;

	TimeVal tv;
	uint64 cycle = get_cycle();
	tv.sec = cycle / CPU_FREQ;
	tv.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;

	*((TimeVal *)kva) = tv;
	return 0;
}

uint64 sys_mmap(uint64 start, uint64 len, int port, int flag, int fd)
{
	struct proc *p = curr_proc();
	if (len == 0)
		return 0;
	if (port & ~0x7)
		return -1;
	if ((port & 0x7) == 0)
		return -1;
	if (start % PAGE_SIZE != 0)
		return -1;

	uint64 npages = PGROUNDUP(len) / PAGE_SIZE;
	if (npages == 0)
		return 0;

	for (uint64 i = 0; i < npages; i++) {
		uint64 va = start + i * PAGE_SIZE;
		if (walkaddr(p->pagetable, va) != 0)
			return -1;
	}

	int perm = PTE_U;
	if (port & 0x1) perm |= PTE_R;
	if (port & 0x2) perm |= PTE_W;
	if (port & 0x4) perm |= PTE_X;

	for (uint64 i = 0; i < npages; i++) {
		uint64 va = start + i * PAGE_SIZE;
		void *pa = kalloc();
		if (pa == 0) {
			uvmunmap(p->pagetable, start, i, 1);
			return -1;
		}
		memset(pa, 0, PAGE_SIZE);
		if (mappages(p->pagetable, va, PAGE_SIZE, (uint64)pa, perm) != 0) {
			kfree(pa);
			uvmunmap(p->pagetable, start, i, 1);
			return -1;
		}
	}

	uint64 end_page = PGROUNDUP(start + npages * PAGE_SIZE) / PAGE_SIZE;
	if (end_page > p->max_page)
		p->max_page = end_page;

	return 0;
}

uint64 sys_munmap(uint64 start, uint64 len)
{
	struct proc *p = curr_proc();
	if (len == 0)
		return 0;
	if (start % PAGE_SIZE != 0)
		return -1;

	uint64 npages = PGROUNDUP(len) / PAGE_SIZE;
	if (npages == 0)
		return 0;

	for (uint64 i = 0; i < npages; i++) {
		uint64 va = start + i * PAGE_SIZE;
		if (walkaddr(p->pagetable, va) == 0)
			return -1;
	}

	uvmunmap(p->pagetable, start, npages, 1);
	return 0;
}

uint64 sys_task_info(struct TaskInfo *ti)
{
	struct proc *p = curr_proc();
	if (ti == 0)
		return -1;

	uint64 kva = useraddr(p->pagetable, (uint64)ti);
	if (kva == 0)
		return -1;

	struct TaskInfo info;
	info.status = p->state;
	uint64 diff = get_cycle() - p->start_time;
	info.time = diff / (CPU_FREQ / 1000);

	for (int i = 0; i < MAX_SYSCALL_NUM; i++) {
		info.syscall_times[i] = p->syscall_times[i];
	}

	*((struct TaskInfo *)kva) = info;
	return 0;
}

extern char trap_page[];

void syscall()
{
	struct proc *p = curr_proc();
	struct trapframe *trapframe = p->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	/*
	* LAB1: you may need to update syscall counter for task info here
	*/
	if (id > 0 && id < MAX_SYSCALL_NUM){
		p->syscall_times[id]++;
	}
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday((TimeVal *)args[0], args[1]);
		break;
	case SYS_mmap:
		ret = sys_mmap(args[0], args[1], (int)args[2], (int)args[3], (int)args[4]);
		break;
	case SYS_munmap:
		ret = sys_munmap(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = curr_proc()->pid;
		break;
	case SYS_task_info:
		ret = sys_task_info((struct TaskInfo *)args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
