/*****************************************************************************

 @(#) $Id$

 -----------------------------------------------------------------------------

 Copyright (c) 2001-2007  OpenSS7 Corporation <http://www.openss7.com/>
 Copyright (c) 1997-2001  Brian F. G. Bidulock <bidulock@openss7.org>

 All Rights Reserved.

 This program is free software; you can redistribute it and/or modify it under
 the terms of the GNU General Public License as published by the Free Software
 Foundation; version 3 of the License.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 details.

 You should have received a copy of the GNU General Public License along with
 this program.  If not, see <http://www.gnu.org/licenses/>, or write to the
 Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

 -----------------------------------------------------------------------------

 U.S. GOVERNMENT RESTRICTED RIGHTS.  If you are licensing this Software on
 behalf of the U.S. Government ("Government"), the following provisions apply
 to you.  If the Software is supplied by the Department of Defense ("DoD"), it
 is classified as "Commercial Computer Software" under paragraph 252.227-7014
 of the DoD Supplement to the Federal Acquisition Regulations ("DFARS") (or any
 successor regulations) and the Government is acquiring only the license rights
 granted herein (the license rights customarily provided to non-Government
 users).  If the Software is supplied to any unit or agency of the Government
 other than DoD, it is classified as "Restricted Computer Software" and the
 Government's rights in the Software are defined in paragraph 52.227-19 of the
 Federal Acquisition Regulations ("FAR") (or any successor regulations) or, in
 the cases of NASA, in paragraph 18.52.227-86 of the NASA Supplement to the FAR
 (or any successor regulations).

 -----------------------------------------------------------------------------

 Commercial licensing and support of this software is available from OpenSS7
 Corporation at a fee.  See http://www.openss7.com/

 -----------------------------------------------------------------------------

 Last Modified $Date$ by $Author$

 -----------------------------------------------------------------------------

 $Log$
 *****************************************************************************/

#ifndef __QUIPU_IMAGE_RWHOD_H__
#define __QUIPU_IMAGE_RWHOD_H__

#ident "@(#) $RCSfile$ $Name$($Revision$) Copyright (c) 2001-2007 OpenSS7 Corporation."

/*	rwhod.h	4.8	83/06/01	*/

/*
 * rwho protocol packet format.
 */
struct outmp {
	char out_line[8];		/* tty name */
	char out_name[8];		/* user id */
	long out_time;			/* time on */
};

struct whod {
	char wd_vers;			/* protocol version # */
	char wd_type;			/* packet type, see below */
	char wd_pad[2];
	int wd_sendtime;		/* time stamp by sender */
	int wd_recvtime;		/* time stamp applied by receiver */
	char wd_hostname[32];		/* hosts's name */
	int wd_loadav[3];		/* load average as in uptime */
	int wd_boottime;		/* time system booted */
	struct whoent {
		struct outmp we_utmp;	/* active tty info */
		int we_idle;		/* tty idle time */
	} wd_we[1024 / sizeof(struct whoent)];
};

#define	WHODVERSION	1
#define	WHODTYPE_STATUS	1	/* host status */

#endif				/* __QUIPU_IMAGE_RWHOD_H__ */