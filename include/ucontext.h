#ifndef __UCONTEXT_H
#define __UCONTEXT_H

typedef unsigned long greg_t, gregset_t[16];
typedef unsigned long freg_t, fpregset_t[16];
typedef struct sigcontext {
    unsigned long oldmask;
    unsigned long gregs[16];
    unsigned long pc, pr, sr;
    unsigned long gbr, mach, macl;
    unsigned long fpregs[16];
    unsigned long xfpregs[16];
    unsigned int fpscr, fpul, ownedfp;
} mcontext_t;

typedef struct {
    void *ss_sp;
    int ss_flags;
    size_t ss_size;
} libucontext_stack_t;

typedef struct ucontext {
    unsigned long uc_flags;
    struct ucontext *uc_link;
    libucontext_stack_t uc_stack;
    mcontext_t uc_mcontext;
} ucontext_t;

int  getcontext(ucontext_t *);
void makecontext(ucontext_t *, void (*)(), int, ...);
int  setcontext(const ucontext_t *);
int  swapcontext(ucontext_t *, const ucontext_t *);

#endif  // __UCONTEXT_H
