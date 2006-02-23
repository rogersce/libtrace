/*
 * This file is part of libtrace
 *
 * Copyright (c) 2004 The University of Waikato, Hamilton, New Zealand.
 * Authors: Daniel Lawson 
 *          Perry Lorier 
 *          
 * All rights reserved.
 *
 * This code has been developed by the University of Waikato WAND 
 * research group. For further information please see http://www.wand.net.nz/
 *
 * libtrace is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * libtrace is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with libtrace; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * $Id$
 *
 */

#define _GNU_SOURCE
#include "config.h"
#include "common.h"
#include "libtrace.h"
#include "libtrace_int.h"
#include "format_helper.h"
#include "wag.h"

#ifdef HAVE_INTTYPES_H
#  include <inttypes.h>
#else
#  error "Can't find inttypes.h - this needs to be fixed"
#endif 

#ifdef HAVE_STDDEF_H
#  include <stddef.h>
#else
# error "Can't find stddef.h - do you define ptrdiff_t elsewhere?"
#endif
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef HAVE_LIMITS_H
#  include <limits.h>
#endif

#ifdef HAVE_SYS_LIMITS_H
#  include <sys/limits.h>
#endif

#ifndef O_LARGEFILE
#define O_LARGEFILE 0
#endif

static struct libtrace_format_t wag;
static struct libtrace_format_t wag_trace;

#define INPUT libtrace->format_data->input
#define OUTPUT libtrace->format_data->output
#define OPTIONS libtrace->format_data->options

struct libtrace_format_data_t {
	union {
                char *path;		/**< information for local sockets */
        } conn_info;
	/** Information about the current state of the input device */
        union {
                int fd;
#if HAVE_ZLIB
                gzFile *file;
#else	
		int file;
#endif
        } input;	
};

struct libtrace_format_data_out_t {
	union {
		char *path;
	} conn_info;
	union {
		struct {
			int level;
			int filemode;
		} zlib;
	} options;
	union {
		int fd;
#if HAVE_ZLIB
		gzFile *file;
#else
		int file;
#endif
	} output;
};

static int wag_init_input(struct libtrace_t *libtrace) {
	struct stat buf;
	libtrace->format_data = (struct libtrace_format_data_t *) 
		calloc(1,sizeof(struct libtrace_format_data_t));
	
	if (stat(libtrace->uridata,&buf) == -1 ) {
		trace_set_err(errno,"stat(%s)",libtrace->uridata);
		return 0;
	}
	if (S_ISCHR(buf.st_mode)) {
				
		INPUT.fd = open(libtrace->uridata, O_RDONLY);

	} else {
		trace_set_err(TRACE_ERR_INIT_FAILED,
				"%s is not a valid char device",
				libtrace->uridata);
		return 0;
		
	}
	return 1;
}

static int wtf_init_input(struct libtrace_t *libtrace) 
{
	libtrace->format_data = (struct libtrace_format_data_t *)
		malloc(sizeof(struct libtrace_format_data_t));

	return 1;
}

static int wtf_start_input(libtrace_t *libtrace)
{
	libtrace->format_data->input.file = trace_open_file(libtrace);

	if (libtrace->format_data->input.file)
		return 1;

	return 0;
}

static int wtf_init_output(struct libtrace_out_t *libtrace) {
	libtrace->format_data = (struct libtrace_format_data_out_t *)
		calloc(1,sizeof(struct libtrace_format_data_out_t));

	return 0;
}

static int wtf_start_output(libtrace_out_t *libtrace) {
	OUTPUT.file = trace_open_file_out(libtrace,
			OPTIONS.zlib.level,
			OPTIONS.zlib.filemode);
	if (!OUTPUT.file) {
		return -1;
	}
	return 0;
}

static int wtf_config_output(struct libtrace_out_t *libtrace, 
		trace_option_output_t option,
		void *value) {
	switch(option) {
#if HAVE_ZLIB
		case TRACE_OPTION_OUTPUT_COMPRESS:
			OPTIONS.zlib.level = *(int*)value;
			assert(OPTIONS.zlib.level>=0 
					&& OPTIONS.zlib.level<=9);
			break;
#else
		case TRACE_OPTION_OUTPUT_COMPRESS:
			/* E feature unavailable */
			trace_set_err(TRACE_ERR_OPTION_UNAVAIL,
					"zlib not supported");
			return -1;
#endif
		default:
			/* E unknown feature */
			trace_set_err(TRACE_ERR_UNKNOWN_OPTION,
					"Unknown option");
			return -1;
	}
}

static int wag_fin_input(struct libtrace_t *libtrace) {
	close(INPUT.fd);
	free(libtrace->format_data);
	return 0;
}

static int wtf_fin_input(struct libtrace_t *libtrace) {
	LIBTRACE_CLOSE(INPUT.file);
	free(libtrace->format_data);
	return 0;
}

static int wtf_fin_output(struct libtrace_out_t *libtrace) {
	LIBTRACE_CLOSE(OUTPUT.file);
	free(libtrace->format_data);
	return 0;
}

static int wag_read(struct libtrace_t *libtrace, void *buffer, size_t len) {
        int numbytes;
        int framesize;
        char *buf_ptr = (char *)buffer;
        int to_read = 0;
        uint16_t magic = 0;
        uint16_t lctr = 0;

        assert(libtrace);

        to_read = sizeof(struct frame_t);

        while (to_read>0) {
          int ret=read(INPUT.fd,buf_ptr,to_read);

          if (ret == -1) {
            if (errno == EINTR || errno==EAGAIN)
              continue;

	    trace_set_err(errno,"read(%s)",libtrace->uridata);
            return -1;
          }

          assert(ret>0);

          to_read = to_read - ret;
          buf_ptr = buf_ptr + ret;
        }


        framesize = ntohs(((struct frame_t *)buffer)->size);
        magic = ntohs(((struct frame_t *)buffer)->magic);

        if (magic != 0xdaa1) {
	  trace_set_err(TRACE_ERR_BAD_PACKET,"magic number bad or missing");
	  return -1;
        }

	/* We should deal.  this is called "snapping", but we don't yet */
	assert(framesize>len);

        buf_ptr = (void*)((char*)buffer + sizeof (struct frame_t));
        to_read = framesize - sizeof(struct frame_t);
        
	while (to_read>0) {
          int ret=read(INPUT.fd,buf_ptr,to_read);

          if (ret == -1) {
            if (errno == EINTR || errno==EAGAIN)
              continue;
	    trace_set_err(errno,"read(%s)",libtrace->uridata);
            return -1;
          }

          to_read = to_read - ret;
          buf_ptr = buf_ptr + ret;
        }
        return framesize;
}


static int wag_read_packet(struct libtrace_t *libtrace, struct libtrace_packet_t *packet) {
	int numbytes;
	
        if (packet->buf_control == TRACE_CTRL_EXTERNAL || !packet->buffer) {
                packet->buf_control = TRACE_CTRL_PACKET;
                packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
	}
	
	
	packet->trace = libtrace;
	
	if ((numbytes = wag_read(libtrace, (void *)packet->buffer, RP_BUFSIZE)) <= 0) {
	    
    		return numbytes;
	}

	
	packet->header = packet->buffer;
	packet->payload=(char*)packet->buffer+trace_get_framing_length(packet);
	return numbytes;
}

static int wtf_read_packet(struct libtrace_t *libtrace, struct libtrace_packet_t *packet) {
	int numbytes;
	void *buffer;
	void *buffer2;
	int framesize;
	int size;

        if (packet->buf_control == TRACE_CTRL_EXTERNAL || !packet->buffer) {
                packet->buf_control = TRACE_CTRL_PACKET;
                packet->buffer = malloc(LIBTRACE_PACKET_BUFSIZE);
        }

	buffer2 = buffer = packet->buffer;
	
	if ((numbytes = LIBTRACE_READ(INPUT.file, buffer, sizeof(struct frame_t))) == -1) {
		trace_set_err(errno,"read(%s,frame_t)",packet->trace->uridata);
		return -1;
	}

	if (numbytes == 0) {
		return 0;
	}

	if (htons(((struct frame_t *)buffer)->magic) != 0xdaa1) {
		trace_set_err(TRACE_ERR_BAD_PACKET,"Insufficient magic");
		return -1;
	}

	framesize = ntohs(((struct frame_t *)buffer)->size);
	buffer2 = (char*)buffer + sizeof(struct frame_t);
	size = framesize - sizeof(struct frame_t);
	assert(size < LIBTRACE_PACKET_BUFSIZE);

	
	if ((numbytes=LIBTRACE_READ(INPUT.file, buffer2, size)) != size) {
		trace_set_err(errno,"read(%s,buffer)",packet->trace->uridata);
		return -1;
	}

	packet->header = packet->buffer;
	packet->payload=(char*)packet->buffer+trace_get_framing_length(packet);
	return framesize;
	
}				
	
static int wtf_write_packet(struct libtrace_out_t *libtrace, const struct libtrace_packet_t *packet) {
	int numbytes =0 ;
	if (packet->trace->format != &wag_trace) {
		trace_set_err(TRACE_ERR_NO_CONVERSION,
				"Cannot convert from wag trace format to %s format yet",
				packet->trace->format->name);
		return -1;
	}

	/* We could just read from packet->buffer, but I feel it is more
	 * technically correct to read from the header and payload pointers
	 */
	if ((numbytes = LIBTRACE_WRITE(OUTPUT.file, packet->header, 
				trace_get_framing_length(packet))) == -1) {
		trace_set_err(errno,"write(%s)",packet->trace->uridata);
		return -1;
	}
	if ((numbytes = LIBTRACE_WRITE(OUTPUT.file, packet->payload, 
				trace_get_capture_length(packet)) == -1)) {
		trace_set_err(errno,"write(%s)",packet->trace->uridata);
		return -1;
	}
	return numbytes;
}

static libtrace_linktype_t wag_get_link_type(const struct libtrace_packet_t *packet __attribute__((unused))) {
	return TRACE_TYPE_80211;
}

static int8_t wag_get_direction(const struct libtrace_packet_t *packet) {
	struct frame_data_rx_t *wagptr = (struct frame_data_rx_t *)packet->buffer;
	if (wagptr->hdr.type == 0) {
		return wagptr->hdr.subtype;
	}
	return -1;
}

static uint64_t wag_get_erf_timestamp(const struct libtrace_packet_t *packet) {
	struct frame_data_rx_t *wagptr = (struct frame_data_rx_t *)packet->buffer;
	uint64_t timestamp = 0;
	timestamp = ((uint64_t)(ntohl(wagptr->ts.secs)) << 32) | (uint64_t)(ntohl(wagptr->ts.subsecs));
	return timestamp;
}

static int wag_get_capture_length(const struct libtrace_packet_t *packet) {
	struct frame_data_rx_t *wagptr = (struct frame_data_rx_t *)packet->buffer;
	return ntohs(wagptr->hdr.size);
}

static int wag_get_wire_length(const struct libtrace_packet_t *packet) {
	struct frame_data_rx_t *wagptr = (struct frame_data_rx_t *)packet->buffer;
	return ntohs(wagptr->hdr.size);
}

static int wag_get_framing_length(const struct libtrace_packet_t *packet) {
	return sizeof(struct frame_data_rx_t);
}

static int wag_get_fd(const libtrace_t *trace) {
	return trace->format_data->input.fd;
}

static void wag_help() {
	printf("wag format module: $Revision$\n");
	printf("Supported input URIs:\n");
	printf("\twag:/dev/wagn\n");
	printf("\n");
	printf("\te.g.: wag:/dev/wag0\n");
	printf("\n");
	printf("Supported output URIs:\n");
	printf("\tNone\n");
	printf("\n");
}

static void wtf_help() {
	printf("wag trace format module: $Revision$\n");
	printf("Supported input URIs:\n");
	printf("\twtf:/path/to/trace.wag\n");
	printf("\twtf:/path/to/trace.wag.gz\n");
	printf("\n");
	printf("\te.g.: wtf:/tmp/trace.wag.gz\n");
	printf("\n");
	printf("Supported output URIs:\n");
	printf("\twtf:/path/to/trace.wag\n");
	printf("\twtf:/path/to/trace.wag.gz\n");
	printf("\n");
	printf("\te.g.: wtf:/tmp/trace.wag.gz\n");
	printf("\n");
}

static struct libtrace_format_t wag = {
	"wag",
	"$Id$",
	TRACE_FORMAT_WAG,
	wag_init_input,			/* init_input */	
	NULL,				/* config_input */
	NULL,				/* start_input */
	NULL,				/* pause_input */
	NULL,				/* init_output */
	NULL,				/* config_output */
	NULL,				/* start_output */
	wag_fin_input,			/* fin_input */
	NULL,				/* fin_output */
	wag_read_packet,		/* read_packet */
	NULL,				/* write_packet */
	wag_get_link_type,		/* get_link_type */
	wag_get_direction,		/* get_direction */
	NULL,				/* set_direction */
	wag_get_erf_timestamp,		/* get_erf_timestamp */
	NULL,				/* get_timeval */
	NULL,				/* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
	wag_get_capture_length,		/* get_capture_length */
	wag_get_wire_length,		/* get_wire_length */
	wag_get_framing_length,		/* get_framing_length */
	NULL,				/* set_capture_length */
	wag_get_fd,			/* get_fd */
	trace_event_device,		/* trace_event */
	wag_help			/* help */
};

/* wtf stands for Wag Trace Format */

static struct libtrace_format_t wag_trace = {
        "wtf",
        "$Id$",
        TRACE_FORMAT_WAG,
	wtf_init_input,                 /* init_input */
	NULL,				/* config input */
	wtf_start_input,		/* start input */
	NULL,				/* pause_input */
        wtf_init_output,                /* init_output */
        wtf_config_output,              /* config_output */
	wtf_start_output,		/* start output */
        wtf_fin_input,                  /* fin_input */
        wtf_fin_output,                 /* fin_output */
        wtf_read_packet,                /* read_packet */
        wtf_write_packet,               /* write_packet */
        wag_get_link_type,              /* get_link_type */
        wag_get_direction,              /* get_direction */
        NULL,                           /* set_direction */
        wag_get_erf_timestamp,          /* get_erf_timestamp */
        NULL,                           /* get_timeval */
        NULL,                           /* get_seconds */
	NULL,				/* seek_erf */
	NULL,				/* seek_timeval */
	NULL,				/* seek_seconds */
        wag_get_capture_length,         /* get_capture_length */
        wag_get_wire_length,            /* get_wire_length */
        wag_get_framing_length,         /* get_framing_length */
        NULL,                           /* set_capture_length */
        wag_get_fd,                     /* get_fd */
        trace_event_trace,              /* trace_event */
        wtf_help                        /* help */
};


void __attribute__((constructor)) wag_constructor() {
	register_format(&wag);
	register_format(&wag_trace);
}
