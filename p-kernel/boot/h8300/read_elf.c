/*
 * read_elf.c
 *
 *  Created on: 2011/04/25
 *      Author: mizu
 */

#include "read_elf.h"
#include "3069f_sci.h"
#include <string.h>

char* gen_xval(unsigned long val, int column)
{
	int i;
	static char buf[9];
	char* p;

//	for(i = 0; i < 9; i++)
//		buf[i] = 0;

	p = buf + sizeof buf - 1;
	*(p--) = 0x00;

	if(!val && !column)
		column++;

	while(val || column)
	{
		*(p--) = "0123456789abcdef"[val & 0x0f];
		val >>= 4;
		if(column) column--;
	}
	return buf;
}

static int elf_check(struct elf_header *header)
{
	if(memcmp(header->id.magic, "\x7f" "ELF", 4))
		return -1;

	// ヘッダ情報チェック
	if(header->id.class		!= 1) return -1;	// ELF32
	if(header->id.format	!= 2) return -1;	// Big endian
	if(header->id.version	!= 1) return -1;	// version 1
	if(header->type			!= 2) return -1;	// Exe file
	if(header->version		!= 1) return -1;	// version 1

	// H8アーキテクチャ Exe file
	if((header->arch != 46) && (header->arch != 47)) return -1;

	return 0;
}

// elfをRAMへ展開する
char* elf_develop(char* p)
{
	struct elf_header* header;
	struct elf_program_header *program;
	int i;
	header = (struct elf_header*) p;


	if(elf_check(header))
	{
		sci_write_str(SCI_NO_1, "It's not ELF...\r\n");
		return 0;
	}else
	{
		sci_write_str(SCI_NO_1, "It's ELF Data!!\r\n");
	}

	sci_write_str(SCI_NO_1, "EntryPoint  >>> ");
	sci_write_str(SCI_NO_1, gen_xval(header->entry_point, 8));

	for (i = 0; i < header->program_header_num; i++)
	{

		program = (struct elf_program_header*) ((char*) header
				+ header->program_header_offset + header->program_header_size
				* i);

		sci_write_str(SCI_NO_1, "\r\noffset        -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->offset, 			6));
		sci_write_str(SCI_NO_1, "\r\nvirtual_addr  -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->virtual_addr, 	8));
		sci_write_str(SCI_NO_1, "\r\nphysical_addr -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->physical_addr, 	8));
		sci_write_str(SCI_NO_1, "\r\nfile_size     -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->file_size, 		5));
		sci_write_str(SCI_NO_1, "\r\nmemory_size   -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->memory_size, 		5));
		sci_write_str(SCI_NO_1, "\r\nflags         -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->flags, 			2));
		sci_write_str(SCI_NO_1, "\r\nalign         -> ");
		sci_write_str(SCI_NO_1, gen_xval(program->align, 			2));
		sci_write_str(SCI_NO_1, "\r\n");

		// ロード可能かどうか
		if (program->type != 1)
		{
			sci_write_str(SCI_NO_1, "can not load...\r\n");
			continue;
		}else
		{
			sci_write_str(SCI_NO_1, "loading...\r\n");
		}

		sci_write_str(SCI_NO_1, "program headers devloping...\r\n");
		// メモリへセクション展開
		memcpy((char*) program->physical_addr, (char*) header + program->offset, program->file_size);
		sci_write_str(SCI_NO_1, "program headers devloped...\r\n");

		sci_write_str(SCI_NO_1, "initing...\r\n");
		// メモリサイズとファイルサイズの隙間を0で埋める
		memset((char*) program->physical_addr + program->file_size, 0, program->memory_size - program->file_size);
		sci_write_str(SCI_NO_1, "inited...\r\n");

	}

	return (char*)header->entry_point;
}

int elf_analysis(char* p)
{
	struct elf_header* header;
	int i;
	
	header = (struct elf_header*) p;

	for (i = 0; i < header->section_header_num; i++)
	{

		struct elf_program_header *program =
				(struct elf_program_header*) ((char*) header
						+ header->program_header_offset
						+ header->program_header_size * i);

		// ロード可能かどうか
		if (program->type != 1)
			return -1;

	}

	return 0;


//	sci_write(SCI1, );
}

int elf_read(char* p)
{
	struct elf_header* header;

	header = (struct elf_header*)p;

	// ELFかチェック
	if(elf_check(header))
	{
		return -1;
	}

	return 0;

}
