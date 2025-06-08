#ifndef _SETJMP_H_
#define _SETJMP_H_

typedef struct {
	unsigned long regs[16];
} jmp_buf[1];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif /* _SETJMP_H_ */