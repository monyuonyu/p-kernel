CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -fPIC -O2
AR = ar
ARFLAGS = rcs

INCLUDE_DIR = ../../include/lib/libc
CTYPE_DIR = ctype
STRING_DIR = string
STDLIB_DIR = stdlib
MATH_DIR = math
STDIO_DIR = stdio
TIME_DIR = time
ERRNO_DIR = errno

# Object files
CTYPE_OBJS = $(CTYPE_DIR)/isalnum.o $(CTYPE_DIR)/isalpha.o $(CTYPE_DIR)/iscntrl.o \
             $(CTYPE_DIR)/isdigit.o $(CTYPE_DIR)/isgraph.o $(CTYPE_DIR)/islower.o \
             $(CTYPE_DIR)/isprint.o $(CTYPE_DIR)/ispunct.o $(CTYPE_DIR)/isspace.o \
             $(CTYPE_DIR)/isupper.o $(CTYPE_DIR)/isxdigit.o $(CTYPE_DIR)/toascii.o \
             $(CTYPE_DIR)/tolower.o $(CTYPE_DIR)/toupper.o

STRING_OBJS = $(STRING_DIR)/memchr.o $(STRING_DIR)/memcmp.o $(STRING_DIR)/memcpy.o \
              $(STRING_DIR)/memmove.o $(STRING_DIR)/memset.o $(STRING_DIR)/strcat.o \
              $(STRING_DIR)/strchr.o $(STRING_DIR)/strcmp.o $(STRING_DIR)/strcpy.o \
              $(STRING_DIR)/strlen.o $(STRING_DIR)/strncat.o $(STRING_DIR)/strncmp.o \
              $(STRING_DIR)/strncpy.o $(STRING_DIR)/strrchr.o $(STRING_DIR)/strstr.o \
              $(STRING_DIR)/strcoll.o $(STRING_DIR)/strcspn.o $(STRING_DIR)/strspn.o \
              $(STRING_DIR)/strpbrk.o $(STRING_DIR)/strerror.o $(STRING_DIR)/strtok.o \
              $(STRING_DIR)/strxfrm.o

STDLIB_OBJS = $(STDLIB_DIR)/abs.o $(STDLIB_DIR)/atoi.o $(STDLIB_DIR)/atol.o \
              $(STDLIB_DIR)/atof.o $(STDLIB_DIR)/div.o $(STDLIB_DIR)/labs.o \
              $(STDLIB_DIR)/ldiv.o $(STDLIB_DIR)/rand.o $(STDLIB_DIR)/strtod.o \
              $(STDLIB_DIR)/strtoul.o $(STDLIB_DIR)/abort.o $(STDLIB_DIR)/exit.o \
              $(STDLIB_DIR)/bsearch.o $(STDLIB_DIR)/qsort.o $(STDLIB_DIR)/malloc.o

MATH_OBJS = $(MATH_DIR)/fabs.o $(MATH_DIR)/sqrt.o $(MATH_DIR)/floor_ceil.o \
            $(MATH_DIR)/trigonometry.o $(MATH_DIR)/exponential.o \
            $(MATH_DIR)/hyperbolic.o $(MATH_DIR)/fmod.o

STDIO_OBJS = $(STDIO_DIR)/printf.o

TIME_OBJS = $(TIME_DIR)/time.o $(TIME_DIR)/mktime.o

ERRNO_OBJS = $(ERRNO_DIR)/errno.o

ALL_OBJS = $(CTYPE_OBJS) $(STRING_OBJS) $(STDLIB_OBJS) $(MATH_OBJS) \
           $(STDIO_OBJS) $(TIME_OBJS) $(ERRNO_OBJS)

TARGET = libc.a

all: $(TARGET)

$(TARGET): $(ALL_OBJS)
	$(AR) $(ARFLAGS) $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) -c $< -o $@

clean:
	rm -f $(ALL_OBJS) $(TARGET)

install: $(TARGET)
	cp $(TARGET) /usr/local/lib/
	cp -r ../../include/lib/libc/* /usr/local/include/

.PHONY: all clean install