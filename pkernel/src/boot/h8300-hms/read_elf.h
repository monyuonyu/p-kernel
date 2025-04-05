/*
 * read_elf.h
 *
 *  Created on: 2011/04/25
 *      Author: mizu
 */

#ifndef READ_ELF_H_
#define READ_ELF_H_

struct elf_header
{
	struct
	{
		unsigned char magic[4];
		unsigned char class;
		unsigned char format;
		unsigned char version;
		unsigned char abi;
		unsigned char abi_version;
		unsigned char reserve[7];
	}id;

	short type;
	short arch;
	long version;
	long entry_point;
	long program_header_offset;
	long section_header_offset;
	long flags;
	short header_size;
	short program_header_size;
	short program_header_num;
	short section_header_size;
	short section_header_num;
	short section_name_index;
};

struct elf_program_header
{
	long type;
	long offset;
	long virtual_addr;
	long physical_addr;
	long file_size;
	long memory_size;
	long flags;
	long align;
};


int elf_read(char* p);
int elf_analysis(char* p);
char* elf_develop(char* p);

#endif /* READ_ELF_H_ */
