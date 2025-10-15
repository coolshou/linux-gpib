/***************************************************************************
                          lib/pass_control.c
                             -------------------

	copyright            : (C) 2002 by Frank Mori Hess
    email                : fmhess@users.sourceforge.net
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "ib_internal.h"

int my_pass_control(ibConf_t *conf, unsigned int pad, int sad)
{
	uint8_t cmd;
	int retval;
	int i;

	i = InternalReceiveSetup(conf, conf->settings.usec_timeout, packAddress(pad, sad));

	cmd = TCT;
	retval = my_ibcmd(conf, conf->settings.usec_timeout, &cmd, i);
	if (retval < 0)
		return retval;

	retval = internal_ibgts(conf, 0);

	return 0;
}

int ibpct(int ud)
{
	ibConf_t *conf;
	int retval;

	conf = enter_library(ud);
	if (!conf)
		return exit_library(ud, 1);

	if (conf->is_interface)	{
		setIberr(EARG);
		return exit_library(ud, 1);
	}

	retval = my_pass_control(conf, conf->settings.pad, conf->settings.sad);
	if (retval < 0)
		return exit_library(ud, 1);

	return exit_library(ud, 0);
}

void PassControl(int boardID, Addr4882_t address)
{
	ibConf_t *conf;
	int retval;

	conf = enter_library(boardID);
	if (!conf) {
		exit_library(boardID, 1);
		return;
	}

	if (!addressIsValid(address)) {
		exit_library(boardID, 1);
		return;
	}

	if (!conf->is_interface) {
		setIberr(EARG);
		exit_library(boardID, 1);
		return;
	}

	retval = my_pass_control(conf, extractPAD(address), extractSAD(address));
	if (retval < 0)	{
		exit_library(boardID, 1);
		return;
	}

	exit_library(boardID, 0);
}
