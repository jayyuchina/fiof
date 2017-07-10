/*
 * Copyright (c) <2008>, Sun Microsystems, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the  nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY SUN MICROSYSTEMS, INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL SUN MICROSYSTEMS, INC. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/*
 * Summary: Constants used by to implement the binary protocol.
 *
 * Copy: See Copyright for the status of this software.
 *
 * Author: Trond Norbye <trond.norbye@sun.com>
 */

#ifndef PROTOCOL_BINARY_H
#define PROTOCOL_BINARY_H
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <netinet/in.h>
/**
 * This file contains definitions of the constants and packet formats
 * defined in the binary specification. Please note that you _MUST_ remember
 * to convert each multibyte field to / from network byte order to / from
 * host order.
 */
#ifdef __cplusplus
extern "C"
{
#endif

    /**
     * Definition of the legal "magic" values used in a packet.
     * See section 3.1 Magic byte
     */
    typedef enum {
        PROTOCOL_BINARY_REQ = 0x80,
        PROTOCOL_BINARY_RES = 0x81
    } protocol_binary_magic;

    /**
     * Definition of the valid response status numbers.
     * See section 3.2 Response Status   not changed.
     */
    typedef enum {
        PROTOCOL_BINARY_RESPONSE_SUCCESS = 0x00,
        PROTOCOL_BINARY_RESPONSE_EINVAL = 0x01,
        PROTOCOL_BINARY_RESPONSE_UNKNOWN_COMMAND = 0x81,
        PROTOCOL_BINARY_RESPONSE_ENOMEM = 0x82
    } protocol_binary_response_status;

    /**
     * Defintion of the different command opcodes.
     * See section 3.3 Command Opcodes
     */
    typedef enum {
        PROTOCOL_BINARY_CMD_CREAT = 0x00,
        PROTOCOL_BINARY_CMD_CREAT64 = 0x01,
        PROTOCOL_BINARY_CMD_OPEN = 0x02,
        PROTOCOL_BINARY_CMD_OPEN64 = 0x03,
        PROTOCOL_BINARY_CMD_CLOSE = 0x04,
        PROTOCOL_BINARY_CMD_WRITE = 0x05,
        PROTOCOL_BINARY_CMD_READ = 0x06,
        PROTOCOL_BINARY_CMD_LSEEK = 0x07,
        PROTOCOL_BINARY_CMD_LSEEK64 = 0x08,
        PROTOCOL_BINARY_CMD_PREAD = 0x09,
        PROTOCOL_BINARY_CMD_PREAD64 = 0x0a,
        PROTOCOL_BINARY_CMD_PWRITE = 0x0b,
        PROTOCOL_BINARY_CMD_PWRITE64 = 0x0c,
        PROTOCOL_BINARY_CMD_READV = 0x0d,
        PROTOCOL_BINARY_CMD_WRITEV = 0x0e,
        PROTOCOL_BINARY_CMD___FXSTAT = 0x0f,
        PROTOCOL_BINARY_CMD___FXSTAT64 = 0x10,
        PROTOCOL_BINARY_CMD___LXSTAT = 0x11,
        PROTOCOL_BINARY_CMD___LXSTAT64 = 0x12,
        PROTOCOL_BINARY_CMD___XSTAT = 0x13,
        PROTOCOL_BINARY_CMD___XSTAT64 = 0x14,
        PROTOCOL_BINARY_CMD_MMAP = 0x15,
        PROTOCOL_BINARY_CMD_MMAP64 = 0x16,
        PROTOCOL_BINARY_CMD_FOPEN = 0x17,
        PROTOCOL_BINARY_CMD_FOPEN64 = 0x18,
        PROTOCOL_BINARY_CMD_FCLOSE = 0x19,
        PROTOCOL_BINARY_CMD_FREAD = 0x1a,
        PROTOCOL_BINARY_CMD_FWRITE = 0x1b,
        PROTOCOL_BINARY_CMD_FSEEK = 0x1c,
        PROTOCOL_BINARY_CMD_FSYNC = 0x1d,
        PROTOCOL_BINARY_CMD_FDATASYNC = 0x1e,
        PROTOCOL_BINARY_CMD_AIO_READ = 0x20,
        PROTOCOL_BINARY_CMD_AIO_READ64 = 0x21,
        PROTOCOL_BINARY_CMD_AIO_WRITE = 0x22,
        PROTOCOL_BINARY_CMD_AIO_WRITE64 = 0x23,
        PROTOCOL_BINARY_CMD_LIO_LISTIO = 0x24,
        PROTOCOL_BINARY_CMD_LIO_LISTIO64 = 0x25,
        PROTOCOL_BINARY_CMD_AIO_RETURN = 0x26,
        PROTOCOL_BINARY_CMD_AIO_RETURN64 = 0x27,
    }protocol_binary_command;

    /**
     * Definition of the data types in the packet
     * See section 3.4 Data Types
     */
    typedef enum {
        PROTOCOL_BINARY_RAW_BYTES = 0x00
    } protocol_binary_datatypes;

    /**
     * Definition of the header structure for a request packet.
     * See section 2
     */
    typedef union {
        struct {
            uint8_t magic;
            uint8_t opcode;
			uint8_t isfrag;		/*how many packages do this request contain ?*/
			uint8_t frag_id;
			uint8_t request_id;	/*one request may distibute in some packets, use this id to recognise*/
            uint8_t para_num;
			uint16_t reserved;
            uint64_t body_len;	/*the length of the packet body without this header*/
			uint64_t request_len;
			uint64_t frag_offset;
			uint64_t para_len;	
            uint64_t para1_len;
            uint64_t para2_len;
            uint64_t para3_len;
	        uint64_t para4_len;
			uint64_t para5_len;
			uint64_t para6_len;
        } request;
        uint8_t bytes[88];
    } protocol_binary_request_header;

    /**
     * Definition of the header structure for a response packet.
     * See section 2
     */
    typedef union {
        struct {
            uint8_t magic;
            uint8_t opcode;
			uint8_t isfrag;		/*is this request too large not to send with one package*/
			uint8_t frag_id;
			uint8_t response_id;	/*one request may distibute in some packets, use this id to recognise*/
            uint8_t para_num;
			uint16_t reserved;
            uint64_t body_len;	/*the length of the packet body without this header*/
			uint64_t response_len;
			uint64_t frag_offset;
			uint64_t para_len;	
            uint64_t para1_len;	/*return value*/
            uint64_t para2_len;
            uint64_t para3_len;
	        uint64_t para4_len;
			uint64_t para5_len;
			uint64_t para6_len;
        } response;
        uint8_t bytes[88];
    } protocol_binary_response_header;

    /**
     * Definition of a request-packet containing no extras
     */
    typedef union {
        struct {
            protocol_binary_request_header header;
        } message;
        uint8_t bytes[sizeof(protocol_binary_request_header)];
    } protocol_binary_request_no_extras;

    /**
     * Definition of a response-packet containing no extras
     */
    typedef union {
        struct {
            protocol_binary_response_header header;
        } message;
        uint8_t bytes[sizeof(protocol_binary_response_header)];
    } protocol_binary_response_no_extras;



	typedef union {
		struct {
			protocol_binary_response_header header;
			struct {
				uint64_t return_value;
				uint32_t return_errno;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_response_header) + 12];
	} protocol_binary_response;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t mode;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 4];
	} protocol_binary_request_creat;

	typedef protocol_binary_request_creat protocol_binary_request_creat64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t flags;
				uint32_t mode;
				} body;
			} message;
		uint8_t bytes[sizeof (protocol_binary_request_header) + 8];
	} protocol_binary_request_open;

	typedef protocol_binary_request_open protocol_binary_request_open64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t fd;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 4];
	} protocol_binary_request_close;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t fd;
				uint64_t count;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 12];
	} protocol_binary_request_write;
	
	typedef protocol_binary_request_write protocol_binary_request_read;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t fd;
				uint64_t offset;
				uint32_t whence;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 16];
	} protocol_binary_request_lseek;	

	typedef protocol_binary_request_lseek protocol_binary_request_lseek64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t fd;
				uint64_t count;
				uint64_t offset;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 20];
	} protocol_binary_request_pread;	

	typedef protocol_binary_request_pread protocol_binary_request_pread64;
	typedef protocol_binary_request_pread protocol_binary_request_pwrite;
	typedef protocol_binary_request_pread protocol_binary_request_pwrite64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t fd;
				uint32_t iovcnt;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 8];
	} protocol_binary_request_readv;

	typedef protocol_binary_request_readv protocol_binary_request_writev;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t vers;
				uint32_t fd;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 8];
	} protocol_binary_request___fxstat;

	typedef protocol_binary_request___fxstat protocol_binary_request___fxstat64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t vers;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 4];
	} protocol_binary_request___lxstat;

	typedef protocol_binary_request___lxstat protocol_binary_request___lxstat64;
	typedef protocol_binary_request___lxstat protocol_binary_request___xstat64;
	typedef protocol_binary_request___lxstat protocol_binary_request___xstat;

	
	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint64_t length;
				uint32_t prot;
				uint32_t flags;
				uint32_t fd;
				uint64_t offset;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 28];
	} protocol_binary_request_mmap;
	typedef protocol_binary_request_mmap protocol_binary_request_mmap64;

	typedef protocol_binary_request_no_extras protocol_binary_request_fopen;
	typedef protocol_binary_request_no_extras protocol_binary_request_fopen64;
	typedef protocol_binary_request_no_extras protocol_binary_request_fclose;
	
	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint64_t size;
				uint64_t nmemb;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 16];
	} protocol_binary_request_fread;

	typedef protocol_binary_request_fread protocol_binary_request_fwrite;

	
	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint64_t offset;
				uint32_t whence;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 12];
	} protocol_binary_request_fseek;

	typedef protocol_binary_request_close protocol_binary_request_fsync;
	typedef protocol_binary_request_close protocol_binary_request_fdatasync;

	typedef protocol_binary_request_no_extras protocol_binary_request_aio_write;
	typedef protocol_binary_request_no_extras protocol_binary_request_aio_write64;
	typedef protocol_binary_request_no_extras protocol_binary_request_aio_read;
	typedef protocol_binary_request_no_extras protocol_binary_request_aio_read64;

	typedef union {
		struct {
			protocol_binary_request_header header;
			struct {
				uint32_t mode;
				uint32_t nitems;
			} body;
		} message;
		uint8_t bytes[sizeof(protocol_binary_request_header) + 8];
	} protocol_binary_request_lio_listio;

	typedef protocol_binary_request_lio_listio protocol_binary_request_lio_listio64;
	typedef protocol_binary_request_no_extras protocol_binary_request_aio_return;
	typedef protocol_binary_request_no_extras protocol_binary_request_aio_return64;


#ifdef __cplusplus
}
#endif
#endif /* PROTOCOL_BINARY_H */


