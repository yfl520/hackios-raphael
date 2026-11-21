/*
 * Copyright (c) 2015 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * HakiOS: HFS Plus built into the kernel (bsd/hfs, from Apple's hfs-627.100.6) rather than a kext, for the data
 * volume on the phone's flash.  FIFO, CONFIG_PROTECT and NAMEDSTREAMS are the kernel's own options (config/MASTER);
 * what is HFS's alone is decided here.
 */
#ifndef _hfs_config_
#define _hfs_config_
#define HFS_COMPRESSION 1       /* (the code does not build without it; decmpfs is the kernel's) */
#ifndef QUOTA
#define QUOTA 0
#endif
#define CONFIG_SECLUDED_RENAME 0
/* content protection's keys are AppleKeyStore's, which this kernel does not have (bsd/hfs/hfs_builtin.c) */
#undef CONFIG_PROTECT
#define CONFIG_PROTECT 0
#endif /* defined(_hfs_config_) */
