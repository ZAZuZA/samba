/*
   Unix SMB/Netbios implementation.
   Samba select/poll implementation
   Copyright (C) Andrew Tridgell 1992-1998

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _select_h_
#define _select_h_

/* The following definitions come from lib/util/select.c  */

void sys_select_signal(char c);
int sys_select(int maxfd, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *tval);
int sys_select_intr(int maxfd, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *tval);

#endif