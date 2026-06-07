BOOTSRC		=	bootsct
BOOTLD		=	bootld
SEGLS		=	SEGLS.ls

img:
	#ブートセクタ 134->512byte 1sec
	gcc $(BOOTSRC).c -c -o $(BOOTSRC).o
	ld $(BOOTSRC).o -T ../../common/$(SEGLS) -o $(BOOTSRC).bin
	../../tool/FillNop 512 $(BOOTSRC).bin
	../../tool/BootSigned $(BOOTSRC).bin
	#ブートローダ 2071->2560byte 5sec
	gcc $(BOOTLD).c -c -o $(BOOTLD).o
	ld $(BOOTLD).o -T ../../common/$(SEGLS) -o $(BOOTLD).bin
	../../tool/FillNop 512 bootld.bin
	#ブートデータ生成 512+2560 = 3072byte 6sec
	../../tool/link $(BOOTSRC).bin $(BOOTLD).bin boot.bin

img2:	bootsct.o biosCall16.o
	ld bootsct.o biosCall16.o -T SEGLS.ls -o bootsct.bin
	../../tool/FillNop 512 $(BOOTSRC).bin
	../../tool/BootSigned $(BOOTSRC).bin
	
img3:bootld.o biosCall16.o lowCall16.o 32mode.o
	ld bootld.o biosCall16.o lowCall16.o 32mode.o -T SEGLS.ls -o bootld.bin
	../../tool/FillNop 512 bootld.bin
	
img4:
	../../tool/link bootsct.bin bootld.bin boot.bin

img5: img2 img3 img4
	
.c.o:
	gcc -c $<
	

run:
	qemu -fda boot.bin
	
debug:
	qemu -S -s -fda boot.bin

del:
	rm -f *.o
	rm -f *.bin
	rm -f $(BOOTSRC).s

rundel:
	make img
	make run
	make del

as:
	gcc $(BOOTSRC).c -S
	gcc $(BOOTLD).c -S
