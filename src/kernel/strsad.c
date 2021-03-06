/*****************************************************************************

 @(#) File: src/kernel/strsad.c

 -----------------------------------------------------------------------------

 Copyright (c) 2008-2019  Monavacon Limited <http://www.monavacon.com/>
 Copyright (c) 2001-2008  OpenSS7 Corporation <http://www.openss7.com/>
 Copyright (c) 1997-2001  Brian F. G. Bidulock <bidulock@openss7.org>

 All Rights Reserved.

 This program is free software: you can redistribute it and/or modify it under
 the terms of the GNU Affero General Public License as published by the Free
 Software Foundation, version 3 of the license.

 This program is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU Affero General Public License for more
 details.

 You should have received a copy of the GNU Affero General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>, or
 write to the Free Software Foundation, Inc., 675 Mass Ave, Cambridge, MA
 02139, USA.

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

 *****************************************************************************/

static char const ident[] = "src/kernel/strsad.c (" PACKAGE_ENVR ") " PACKAGE_DATE;

#ifdef NEED_LINUX_AUTOCONF_H
#include NEED_LINUX_AUTOCONF_H
#endif
#include <linux/version.h>
#include <linux/module.h>
#include <linux/init.h>
#if defined HAVE_KINC_LINUX_SECURITY_H
#include <linux/security.h>	/* avoid ptrace conflict */
#endif

#include "sys/strdebug.h"

#include <sys/stream.h>
#include <sys/strconf.h>
#include <sys/strsubr.h>
#include <sys/ddi.h>
#include <sys/sad.h>

#include "sys/config.h"
#include "strsched.h"		/* for ap_alloc/ap_put */
#include "strsad.h"		/* extern verification */

/* we want macro versions of these */

#undef getmajor
#define getmajor(__ino) (((__ino)>>16)&0x0000ffff)

#undef getminor
#define getminor(__ino) (((__ino)>>0)&0x0000ffff)

#undef makedevice
#define makedevice(__maj,__min) ((((__maj)<<16)&0xffff0000)|(((__min)<<0)&0x0000ffff))

#if	defined DEFINE_SPINLOCK
STATIC DEFINE_SPINLOCK(apush_lock);
#elif	defined __SPIN_LOCK_UNLOCKED
STATIC spinlock_t apush_lock = __SPIN_LOCK_UNLOCKED(apush_lock);
#elif	defined SPIN_LOCK_UNLOCKED
STATIC spinlock_t apush_lock = SPIN_LOCK_UNLOCKED;
#else
#error cannot initialize spin locks
#endif

STATIC struct apinfo *
__autopush_find(struct cdevsw *cdev, minor_t minor)
{
	struct list_head *pos;
	struct apinfo *api = NULL;

	ensure(cdev->d_apush.next, INIT_LIST_HEAD(&cdev->d_apush));
	list_for_each(pos, &cdev->d_apush) {
		api = list_entry(pos, struct apinfo, api_more);

		if (minor >= api->api_sap.sap_minor && minor <= api->api_sap.sap_lastminor)
			break;
		api = NULL;
	}
	return (api);
}

STATIC struct apinfo *
__autopush_next(struct cdevsw *cdev, minor_t minor)
{
	struct list_head *pos;
	struct apinfo *api = NULL, *ani = NULL;
	minor_t next = minor;

	ensure(cdev->d_apush.next, INIT_LIST_HEAD(&cdev->d_apush));
	list_for_each(pos, &cdev->d_apush) {
		api = list_entry(pos, struct apinfo, api_more);

		if (minor >= api->api_sap.sap_minor && minor <= api->api_sap.sap_lastminor)
			break;
		if (next == minor) {
			if (api->api_sap.sap_minor > minor) {
				next = api->api_sap.sap_minor;
				ani = api;
			}
		} else {
			if (api->api_sap.sap_minor > minor && api->api_sap.sap_minor < next) {
				next = api->api_sap.sap_minor;
				ani = api;
			}
		}
		api = NULL;
	}
	return (api ?: ani);
}

STATIC int
__autopush_add(struct cdevsw *cdev, struct strapush *sap)
{
	struct apinfo *api;
	int err;

	err = -EEXIST;
	if ((api = __autopush_find(cdev, sap->sap_minor)) != NULL) {
		_ptrace(("Error path taken! EEXIST\n"));
		goto error;
	}
	err = -ENOSR;
	if ((api = ap_alloc(sap)) == NULL) {
		_ptrace(("Error path taken! ENOSR\n"));
		goto error;
	}
	ensure(cdev->d_apush.next, INIT_LIST_HEAD(&cdev->d_apush));
	list_add_tail(&api->api_more, &cdev->d_apush);
	return (0);
      error:
	return (err);
}

STATIC int
__autopush_del(struct cdevsw *cdev, struct strapush *sap)
{
	struct apinfo *api;
	int err;

	err = -ENODEV;
	if ((api = __autopush_find(cdev, sap->sap_minor)) == NULL) {
		_ptrace(("Error path taken! ENODEV\n"));
		goto error;
	}
	err = -ERANGE;
	if (sap->sap_minor != api->api_sap.sap_minor) {
		_ptrace(("Error path taken! ERANGE\n"));
		goto error;
	}
	ap_put(api);
	return (0);
      error:
	return (err);
}

streams_fastcall struct strapush *
autopush_next(dev_t dev)
{
	unsigned long flags;
	struct cdevsw *cdev;
	struct apinfo *api = NULL;

	cdev = cdrv_get(getmajor(dev));
	if (cdev == NULL)
		goto notfound;
	_printd(("%s: %s: got driver\n", __FUNCTION__, cdev->d_name));
	/* XXX: do these locks have to be so severe? */
	streams_spin_lock(&apush_lock, flags);
	if ((api = __autopush_next(cdev, getminor(dev))) != NULL)
		ap_get(api);
	streams_spin_unlock(&apush_lock, flags);
	_printd(("%s: %s: putting driver\n", __FUNCTION__, cdev->d_name));
	_ctrace(cdrv_put(cdev));
      notfound:
	return ((struct strapush *) api);
}

EXPORT_SYMBOL_GPL(autopush_next);

streams_fastcall struct strapush *
autopush_find(dev_t dev)
{
	unsigned long flags;
	struct cdevsw *cdev;
	struct apinfo *api = NULL;

	cdev = cdrv_get(getmajor(dev));
	if (cdev == NULL)
		goto notfound;
	_printd(("%s: %s: got driver\n", __FUNCTION__, cdev->d_name));
	/* XXX: do these locks have to be so severe? */
	streams_spin_lock(&apush_lock, flags);
	if ((api = __autopush_find(cdev, getminor(dev))) != NULL)
		ap_get(api);
	streams_spin_unlock(&apush_lock, flags);
	_printd(("%s: %s: putting driver\n", __FUNCTION__, cdev->d_name));
	_ctrace(cdrv_put(cdev));
      notfound:
	return ((struct strapush *) api);
}

EXPORT_SYMBOL_GPL(autopush_find);

streams_fastcall struct strapush *
autopush_search(const char *name, minor_t minor)
{
	char module[FMNAMESZ + 1] = { 0, };
	unsigned long flags;
	struct cdevsw *cdev;
	struct apinfo *api = NULL;

	if (name[0] == '\0')
		goto notfound;

	strncpy(module, name, FMNAMESZ);
	module[FMNAMESZ] = '\0';

	if (!(cdev = cdev_find(module)))
		goto notfound;
	_printd(("%s: %s: got driver\n", __FUNCTION__, cdev->d_name));
	/* XXX: do these locks have to be so severe? */
	streams_spin_lock(&apush_lock, flags);
	if ((api = __autopush_find(cdev, minor)) != NULL)
		ap_get(api);
	streams_spin_unlock(&apush_lock, flags);
	_printd(("%s: %s: putting driver\n", __FUNCTION__, cdev->d_name));
	_ctrace(sdev_put(cdev));
      notfound:
	return ((struct strapush *) api);
}

EXPORT_SYMBOL_GPL(autopush_search);

streams_fastcall int
autopush_add(struct strapush *sap)
{
	unsigned long flags;
	struct cdevsw *cdev;
	int err, k;

	switch (sap->sap_cmd) {
	case SAP_ONE:
	case SAP_CLONE:
		sap->sap_lastminor = sap->sap_minor;
		break;
	case SAP_RANGE:
		err = -ERANGE;
		if (sap->sap_lastminor <= sap->sap_minor) {
			_ptrace(("Error path taken! ERANGE\n"));
			goto error;
		}
		break;
	case SAP_ALL:
		sap->sap_minor = 0;
		sap->sap_lastminor = getminor(makedevice(0, -1UL));
		break;
	default:
		err = -EINVAL;
		_ptrace(("Error path taken! EINVAL\n"));
		goto error;
	}
	err = -EINVAL;
	if (sap->sap_module[0] == '\0')
		if (sap->sap_major == 0
		    || sap->sap_major != getmajor(makedevice(sap->sap_major, 0))) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto error;
		}
	if (sap->sap_minor != getminor(makedevice(0, sap->sap_minor))) {
		_ptrace(("Error path taken! EINVAL\n"));
		goto error;
	}
	if (1 > sap->sap_npush || sap->sap_npush > MAXAPUSH) {
		_ptrace(("Error path taken! EINVAL\n"));
		goto error;
	}
	for (k = 0; k < sap->sap_npush; k++) {
		int len = strnlen(sap->sap_list[k], FMNAMESZ + 1);

		if (len == 0 || len == FMNAMESZ + 1) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto error;
		}
	}
	err = -ENOSTR;
	if (sap->sap_module[0] == '\0')
		cdev = sdev_get(sap->sap_major);
	else {
		cdev = cdev_find(sap->sap_module);
		sap->sap_major = cdev->d_major;
	}
	if (cdev == NULL) {
		_ptrace(("Error path taken! ENOSTR\n"));
		goto error;
	}
	_printd(("%s: %s: got device\n", __FUNCTION__, cdev->d_name));
	/* XXX: do these logs have to be so severe? */
	streams_spin_lock(&apush_lock, flags);
	err = __autopush_add(cdev, sap);
	streams_spin_unlock(&apush_lock, flags);
	_printd(("%s: %s: putting device\n", __FUNCTION__, cdev->d_name));
	_ctrace(sdev_put(cdev));
      error:
	return (err);
}

EXPORT_SYMBOL_GPL(autopush_add);

streams_fastcall int
autopush_del(struct strapush *sap)
{
	unsigned long flags;
	struct cdevsw *cdev;
	int err;

	err = -EINVAL;
	if (sap->sap_module[0] == '\0')
		if (sap->sap_major == 0
		    || sap->sap_major != getmajor(makedevice(sap->sap_major, 0))) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto error;
		}
	if (sap->sap_minor != getminor(makedevice(0, sap->sap_minor))) {
		_ptrace(("Error path taken! EINVAL\n"));
		goto error;
	}
	err = -ENODEV;
	if (sap->sap_module[0] == '\0')
		cdev = sdev_get(sap->sap_major);
	else
		cdev = cdev_find(sap->sap_module);
	if (cdev == NULL) {
		_ptrace(("Error path taken! ENODEV\n"));
		goto error;
	}
	_printd(("%s: %s: got device\n", __FUNCTION__, cdev->d_name));
	/* XXX: do these logs have to be so severe? */
	streams_spin_lock(&apush_lock, flags);
	err = __autopush_del(cdev, sap);
	streams_spin_unlock(&apush_lock, flags);
	_printd(("%s: %s: putting device\n", __FUNCTION__, cdev->d_name));
	_ctrace(sdev_put(cdev));
      error:
	return (err);
}

EXPORT_SYMBOL_GPL(autopush_del);

streams_fastcall int
autopush_vml(struct str_mlist *smp, int nmods)
{
	int rtn = 0, k;

	for (k = 0; k < nmods; k++, smp++) {
		int len;
		struct fmodsw *fmod;

		len = strnlen(smp->l_name, FMNAMESZ + 1);
		if (len == 0 || len == FMNAMESZ + 1) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto einval;
		}
		if ((fmod = fmod_find(smp->l_name)) != NULL) {
			_printd(("%s: %s: got module\n", __FUNCTION__, fmod->f_name));
			_printd(("%s: %s: putting module\n", __FUNCTION__, fmod->f_name));
			fmod_put(fmod);
		} else
			rtn = 1;
	}
	return (rtn);
      einval:
	return (-EINVAL);
}

EXPORT_SYMBOL_GPL(autopush_vml);

streams_fastcall int
apush_set(struct strapush *sap)
{
	if (sap != NULL) {
		switch (sap->sap_cmd) {
		case SAP_CLEAR:
			return autopush_del(sap);
		case SAP_ONE:
		case SAP_RANGE:
		case SAP_ALL:
		case SAP_CLONE:
			return autopush_add(sap);
		}
	}
	_ptrace(("Error path taken! EINVAL\n"));
	return (-EINVAL);
}

EXPORT_SYMBOL_GPL(apush_set);

streams_fastcall int
apush_lst(struct strapush *sap)
{
	if (sap != NULL) {
		struct strapush *ap;
		struct cdevsw *cdev;
		unsigned long flags;

		if (sap->sap_module[0] == '\0')
			if (sap->sap_major == 0
			    || sap->sap_major != getmajor(makedevice(sap->sap_major, 0))) {
				_ptrace(("Error path taken! EINVAL\n"));
				goto einval;
			}
		if (sap->sap_minor != getminor(makedevice(0, sap->sap_minor))) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto einval;
		}
		if (sap->sap_module[0] == '\0')
			cdev = sdev_get(sap->sap_major);
		else
			cdev = cdev_find(sap->sap_module);
		if (cdev == NULL) {
			_ptrace(("Error path taken! ENOSTR\n"));
			goto enostr;
		}
		_printd(("%s: %s: got driver\n", __FUNCTION__, cdev->d_name));
		streams_spin_lock(&apush_lock, flags);
		if ((ap = (struct strapush *) __autopush_next(cdev, sap->sap_minor)))
			ap_get((struct apinfo *) ap);
		streams_spin_unlock(&apush_lock, flags);
		if (ap) {
			*sap = *ap;
			ap_put((struct apinfo *) ap);
		}
		_printd(("%s: %s: putting driver\n", __FUNCTION__, cdev->d_name));
		_ctrace(sdev_put(cdev));
		if (!ap) {
			_ptrace(("Error path taken! ENODEV\n"));
			goto enodev;
		}
		return (0);
	}
      einval:
	return (-EINVAL);
      enostr:
	return (-ENOSTR);
      enodev:
	return (-ENODEV);
}

EXPORT_SYMBOL_GPL(apush_lst);	/* strconf.h LfS specific */

streams_fastcall int
apush_get(struct strapush *sap)
{
	if (sap != NULL) {
		struct strapush *ap;
		struct cdevsw *cdev;
		unsigned long flags;

		if (sap->sap_module[0] == '\0')
			if (sap->sap_major == 0
			    || sap->sap_major != getmajor(makedevice(sap->sap_major, 0))) {
				_ptrace(("Error path taken! EINVAL\n"));
				goto einval;
			}
		if (sap->sap_minor != getminor(makedevice(0, sap->sap_minor))) {
			_ptrace(("Error path taken! EINVAL\n"));
			goto einval;
		}
		if (sap->sap_module[0] == '\0')
			cdev = sdev_get(sap->sap_major);
		else
			cdev = cdev_find(sap->sap_module);
		if (cdev == NULL) {
			_ptrace(("Error path taken! ENOSTR\n"));
			goto enostr;
		}
		_printd(("%s: %s: got driver\n", __FUNCTION__, cdev->d_name));
		streams_spin_lock(&apush_lock, flags);
		if ((ap = (struct strapush *) __autopush_find(cdev, sap->sap_minor)))
			ap_get((struct apinfo *) ap);
		streams_spin_unlock(&apush_lock, flags);
		if (ap) {
			*sap = *ap;
			ap_put((struct apinfo *) ap);
		}
		_printd(("%s: %s: putting driver\n", __FUNCTION__, cdev->d_name));
		_ctrace(sdev_put(cdev));
		if (!ap) {
			_ptrace(("Error path taken! ENODEV\n"));
			goto enodev;
		}
		return (0);
	}
      einval:
	return (-EINVAL);
      enostr:
	return (-ENOSTR);
      enodev:
	return (-ENODEV);
}

EXPORT_SYMBOL_GPL(apush_get);	/* strconf.h LiS specific */

streams_fastcall int
apush_vml(struct str_list *slp)
{
	return autopush_vml(slp->sl_modlist, slp->sl_nmods);
}

EXPORT_SYMBOL_GPL(apush_vml);

/**
 *  autopush: - perform autopush operations on a newly opened stream
 *  @sd: newly opened stream head
 *  @cdev: character device switch table entry (driver) for the stream
 *  @oflag: open flags
 *  @sflag: stream flag (%MODOPEN or %CLONEOPEN or %DRVOPEN)
 *  @crp: pointer to user credentials structure
 */
streams_fastcall int
autopush(struct stdata *sd, struct cdevsw *cdev, dev_t *devp, int oflag, int sflag, cred_t *crp)
{
	struct apinfo *api = NULL;
	int err;

	if (*devp != sd->sd_dev) {	/* redirected clone open */
		if ((api = (typeof(api)) autopush_find(sd->sd_dev)) != NULL)
			if (api->api_sap.sap_cmd != SAP_CLONE)
				api = NULL;
		sd->sd_dev = *devp;	/* stream head delayed this for us */
	} else {
		if ((api = (typeof(api)) autopush_find(sd->sd_dev)) != NULL)
			if (api->api_sap.sap_cmd == SAP_CLONE)
				api = NULL;
	}
	if (api != NULL) {
		int k;

		for (k = 0; k < MAX_APUSH; k++) {
			struct fmodsw *fmod;
			dev_t dev;

			if (api->api_sap.sap_list[k][0] == 0)
				break;
			if (!(fmod = fmod_find(api->api_sap.sap_list[k]))) {
				err = -EIO;
				goto abort_autopush;
			}
			_printd(("%s: %s: found module\n", __FUNCTION__, fmod->f_name));
			dev = *devp;	/* don't change dev nr */
			if (fmod->f_str == NULL) {
				_printd(("%s: %s: putting module\n", __FUNCTION__, fmod->f_name));
				fmod_put(fmod);
				err = -EIO;
				goto abort_autopush;
			}
			if ((err = qattach(sd, fmod, &dev, oflag, sflag, crp))) {
				_printd(("%s: %s: putting module\n", __FUNCTION__, fmod->f_name));
				fmod_put(fmod);
				goto abort_autopush;
			}
			atomic_inc(&fmod->f_count);
		}
	}
	return (0);
      abort_autopush:
	{
		/* detach everything, including the driver */
		queue_t *wq = sd->sd_wq;

		if (wq)
			while (wq->q_next && SAMESTR(wq))
				qdetach(wq->q_next - 1, oflag, crp);
		return (err);
	}
}

EXPORT_SYMBOL_GPL(autopush);
