/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _AUDIOMETRICS_H
#define _AUDIOMETRICS_H

typedef int (*pdm_callback)(void *priv, int index);

static inline void pdm_callback_register(pdm_callback callback, int pdm_number,
					 void *pdm_priv)
{
}

#endif /* _AUDIOMETRICS_H */
