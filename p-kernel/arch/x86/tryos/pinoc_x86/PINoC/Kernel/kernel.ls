OUTPUT_FORMAT(binary)
OUTPUT_ARCH(i386)

MEMORY
{
    ram (rwx) : ORIGIN = 0x5800, LENGTH = 0x700
}

SECTIONS {
	
	.text : {
		*(.text)
	} > ram
	
	.rodata :
	{
		*(.rodata)
	} > ram
	
	.data :
	{
		*(.data)
	} > ram
	
	.bss :
	{
		*(.bss)
	} > ram
}
