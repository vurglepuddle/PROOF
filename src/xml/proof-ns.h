// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * XML namespace for PROOF document extensions (spot inks and similar).
 *
 * Kept out of xml/repr.h so registering it does not rebuild every includer.
 * The URI is provisional until the project has a stable web address; files
 * written with it are read back through the prefix table in repr-util.cpp.
 */
#ifndef SEEN_XML_PROOF_NS_H
#define SEEN_XML_PROOF_NS_H

#define SP_PROOF_NS_URI "urn:x-proof:document:1"
#define SP_PROOF_NS_PREFIX "proof"

#endif // SEEN_XML_PROOF_NS_H
