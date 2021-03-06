/*
 * Copyright (C) 2015 Cumulus Networks, Inc.
 *                    Donald Sharp
 *
 * This file is part of Quagga.
 *
 * Quagga is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * Quagga is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; see the file COPYING; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <zebra.h>

#include <vrf.h>
#include <vty.h>

#include <zserv.h>
#include <zebra_ns.h>
#include <zebra_vrf.h>
#include <router-id.h>

int zsend_vrf_delete (struct zserv *zserv, struct zebra_vrf *zvrf)
{ return 0; }

int zsend_vrf_add (struct zserv *zserv, struct zebra_vrf *zvrf)
{ return 0; }

void router_id_init (struct zebra_vrf *zvrf)
{ return; }
