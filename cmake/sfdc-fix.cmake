# Copyright © 2020 Michal Schulz <michal.schulz@gmx.de>
# https://github.com/michalsc
#
# This Source Code Form is subject to the terms of the
# Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed
# with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

# Run by sfdc() (cmake -DFILE=... -DFROM=... -DTO=... -P): sfdc turns a hyphen of the name of the library into an underscore, inside the files it
# generates too (#include <clib/brcm_dma_protos.h>); the files are named after the module (brcm-dma_protos.h), so the names inside follow.
file(READ ${FILE} CONTENT)
string(REPLACE "${FROM}" "${TO}" CONTENT "${CONTENT}")
file(WRITE ${FILE} "${CONTENT}")
