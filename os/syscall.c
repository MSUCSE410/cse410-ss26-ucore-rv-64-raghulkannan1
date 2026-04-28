#include "syscall.h"
#include "console.h"
#include "defs.h"
#include "loader.h"
#include "syscall_ids.h"
#include "timer.h"
#include "trap.h"
#include "file.h"
#include "fs.h"

uint64 console_write(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	int size = copyinstr(p->pagetable, str, va, MIN(len, MAX_STR_LEN));
	tracef("write size = %d", size);
	for (int i = 0; i < size; ++i) {
		console_putchar(str[i]);
	}
	return len;
}

uint64 console_read(uint64 va, uint64 len)
{
	struct proc *p = curr_proc();
	char str[MAX_STR_LEN];
	tracef("read size = %d", len);
	for (int i = 0; i < len; ++i) {
		int c = consgetc();
		str[i] = c;
	}
	copyout(p->pagetable, va, str, len);
	return len;
}

uint64 sys_write(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_write(va, len);
	case FD_INODE:
		return inodewrite(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
}

uint64 sys_read(int fd, uint64 va, uint64 len)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d\n", fd);
		return -1;
	}
	switch (f->type) {
	case FD_STDIO:
		return console_read(va, len);
	case FD_INODE:
		return inoderead(f, va, len);
	default:
		panic("unknown file type %d\n", f->type);
	}
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

uint64 sys_gettimeofday(uint64 val, int _tz)
{
	struct proc *p = curr_proc();
	uint64 cycle = get_cycle();
	TimeVal t;
	t.sec = cycle / CPU_FREQ;
	t.usec = (cycle % CPU_FREQ) * 1000000 / CPU_FREQ;
	copyout(p->pagetable, val, (char *)&t, sizeof(TimeVal));
	return 0;
}

uint64 sys_getpid()
{
	return curr_proc()->pid;
}

uint64 sys_getppid()
{
	struct proc *p = curr_proc();
	return p->parent == NULL ? IDLE_PID : p->parent->pid;
}

uint64 sys_clone()
{
	debugf("fork!");
	return fork();
}

static inline uint64 fetchaddr(pagetable_t pagetable, uint64 va)
{
	uint64 *addr = (uint64 *)useraddr(pagetable, va);
	return *addr;
}

uint64 sys_exec(uint64 path, uint64 uargv)
{
	struct proc *p = curr_proc();
	char name[MAX_STR_LEN];
	copyinstr(p->pagetable, name, path, MAX_STR_LEN);
	uint64 arg;
	static char strpool[MAX_ARG_NUM][MAX_STR_LEN];
	char *argv[MAX_ARG_NUM];
	int i;
	for (i = 0; uargv && (arg = fetchaddr(p->pagetable, uargv));
	     uargv += sizeof(char *), i++) {
		copyinstr(p->pagetable, (char *)strpool[i], arg, MAX_STR_LEN);
		argv[i] = (char *)strpool[i];
	}
	argv[i] = NULL;
	return exec(name, (char **)argv);
}

uint64 sys_wait(int pid, uint64 va)
{
	struct proc *p = curr_proc();
	int *code = (int *)useraddr(p->pagetable, va);
	return wait(pid, code);
}

uint64 sys_spawn(uint64 va)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_set_priority(long long prio)
{
	// TODO: your job is to complete the sys call
	return -1;
}

uint64 sys_openat(uint64 va, uint64 omode, uint64 _flags)
{
	struct proc *p = curr_proc();
	char path[200];
	copyinstr(p->pagetable, path, va, 200);
	return fileopen(path, omode);
}

uint64 sys_close(int fd)
{
	if (fd < 0 || fd > FD_BUFFER_SIZE)
		return -1;
	struct proc *p = curr_proc();
	struct file *f = p->files[fd];
	if (f == NULL) {
		errorf("invalid fd %d", fd);
		return -1;
	}
	fileclose(f);
	p->files[fd] = 0;
	return 0;
}

int sys_fstat(int fd, uint64 ustat)
{
    struct proc *p = curr_proc();

    // Validate fd
    if (fd < 0 || fd >= FD_BUFFER_SIZE)
        return -1;

    struct file *f = p->files[fd];
    if (f == NULL || f->type != FD_INODE)
        return -1;

    // Validate user pointer
    Stat st;
    uint64 kva = useraddr(p->pagetable, ustat);
    if (kva == 0)
        return -1;

    struct inode *ip = f->ip;

    ilock(ip);

    st.dev   = 0;
    st.ino   = ip->inum;
    st.mode  = (ip->type == T_DIR ? DIR : FILE);
    st.nlink = ip->nlink;     // You MUST add nlink to struct inode

    iunlock(ip);

    // Copy to user
    memmove((void*)kva, &st, sizeof(Stat));
    return 0;
}


int sys_linkat(int olddirfd, uint64 oldpath, int newdirfd, uint64 newpath, uint64 flags)
{
    struct proc *p = curr_proc();
    char old[MAX_STR_LEN], new[MAX_STR_LEN];

    // Copy paths from user
    if (copyinstr(p->pagetable, old, oldpath, sizeof(old)) < 0)
        return -1;
    if (copyinstr(p->pagetable, new, newpath, sizeof(new)) < 0)
        return -1;

    // Lookup old inode
    struct inode *ip = namei(old);
    if (ip == NULL)
        return -1;

    ilock(ip);

    // Cannot link directories
    if (ip->type == T_DIR) {
        iunlockput(ip);
        return -1;
    }

    // Lookup parent of newpath
    char name[DIRSIZ];
    struct inode *dp = nameiparent(new, name);
    if (dp == NULL) {
        iunlockput(ip);
        return -1;
    }

    ilock(dp);

    // Check if name already exists
    struct inode *exists = dirlookup(dp, name, 0);
    if (exists != NULL) {
        iunlockput(dp);
        iput(exists);
        iunlockput(ip);
        return -1;
    }

    // Increase link count
    ip->nlink++;
    iupdate(ip);

    // Create directory entry
    if (dirlink(dp, name, ip->inum) < 0) {
        ip->nlink--;
        iupdate(ip);
        iunlockput(dp);
        iunlockput(ip);
        return -1;
    }

    iunlockput(dp);
    iunlockput(ip);
    return 0;
}


int sys_unlinkat(int dirfd, uint64 path, uint64 flags)
{
    struct proc *p = curr_proc();
    char namebuf[MAX_STR_LEN];

    if (copyinstr(p->pagetable, namebuf, path, sizeof(namebuf)) < 0)
        return -1;

    char name[DIRSIZ];
    struct inode *dp = nameiparent(namebuf, name);
    if (dp == NULL)
        return -1;

    ilock(dp);

    // Lookup inode
    struct inode *ip = dirlookup(dp, name, 0);
    if (ip == NULL) {
        iunlockput(dp);
        return -1;
    }

    ilock(ip);

    // Remove directory entry
    if (dirunlink(dp, name) < 0) {
        iunlockput(ip);
        iunlockput(dp);
        return -1;
    }

    // Decrement link count
    ip->nlink--;
    iupdate(ip);

    // If no more links → delete inode + data blocks
    if (ip->nlink == 0) {
        // This triggers freeing in iput()
        iunlockput(ip);
    } else {
        iunlock(ip);
        iput(ip);
    }

    iunlockput(dp);
    return 0;
}


extern char trap_page[];

void syscall()
{
	struct trapframe *trapframe = curr_proc()->trapframe;
	int id = trapframe->a7, ret;
	uint64 args[6] = { trapframe->a0, trapframe->a1, trapframe->a2,
			   trapframe->a3, trapframe->a4, trapframe->a5 };
	tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0],
	       args[1], args[2], args[3], args[4], args[5]);
	switch (id) {
	case SYS_write:
		ret = sys_write(args[0], args[1], args[2]);
		break;
	case SYS_read:
		ret = sys_read(args[0], args[1], args[2]);
		break;
	case SYS_openat:
		ret = sys_openat(args[0], args[1], args[2]);
		break;
	case SYS_close:
		ret = sys_close(args[0]);
		break;
	case SYS_exit:
		sys_exit(args[0]);
		// __builtin_unreachable();
	case SYS_sched_yield:
		ret = sys_sched_yield();
		break;
	case SYS_gettimeofday:
		ret = sys_gettimeofday(args[0], args[1]);
		break;
	case SYS_getpid:
		ret = sys_getpid();
		break;
	case SYS_getppid:
		ret = sys_getppid();
		break;
	case SYS_clone: // SYS_fork
		ret = sys_clone();
		break;
	case SYS_execve:
		ret = sys_exec(args[0], args[1]);
		break;
	case SYS_wait4:
		ret = sys_wait(args[0], args[1]);
		break;
	case SYS_fstat:
	    ret = sys_fstat(args[0],args[1]);
		break;
	case SYS_linkat:
	    ret = sys_linkat(args[0],args[1],args[2],args[3],args[4]);
		break;
	case SYS_unlinkat:
	    ret = sys_unlinkat(args[0],args[1],args[2]);
		break;
	case SYS_spawn:
		ret = sys_spawn(args[0]);
		break;
	default:
		ret = -1;
		errorf("unknown syscall %d", id);
	}
	trapframe->a0 = ret;
	tracef("syscall ret %d", ret);
}
