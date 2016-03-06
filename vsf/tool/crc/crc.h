/***************************************************************************
 *   Copyright (C) 2009 - 2010 by Simon Qian <SimonQian@SimonQian.com>     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/

#ifndef __CRC_H_INCLUDED__
#define __CRC_H_INCLUDED__

struct crc_t
{
	enum crc_bitlen_t
	{
		CRC_BITLEN_8 = 8,
		CRC_BITLEN_16 = 16,
		CRC_BITLEN_32 = 32,
	} bitlen;
	uint32_t result;
	uint32_t poly;
};

#ifdef VSFCFG_STANDALONE_MODULE
#define CRC_MODNAME							"vsf.tool.crc"

struct crc_modifs_t
{
	uint32_t (*calc)(struct crc_t *crc, void *buff, uint32_t num);
};

void crc_modexit(struct vsf_module_t*);
vsf_err_t crc_modinit(struct vsf_module_t*, struct app_hwcfg_t const*);

#define CRC_MOD								\
	((struct crc_modifs_t *)vsf_module_load(CRC_MODNAME, true))
#define crc_calc							CRC_MOD->calc

#else
uint32_t crc_calc(struct crc_t *crc, void *buff, uint32_t num);
#endif

#endif	// __CRC_H_INCLUDED__
