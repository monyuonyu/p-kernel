MAINSRC		=	main
LS		=	kernel.ls
OBJS	= main.o lowCall32.o vector.o

.c.o:
	gcc -c $< -nostdlib -nostdinc -O0

img: $(OBJS)
	#メインカーネル 573->1024byte
#	gcc $(MAINSRC).c -nostdlib -nostdinc -c -o $(MAINSRC).o
	ld $(OBJS) -T $(LS) -o main.bin
	../../tool/FillNop 512 $(MAINSRC).bin

as:
	gcc main.c -S
	gcc lowCall32.c -S
