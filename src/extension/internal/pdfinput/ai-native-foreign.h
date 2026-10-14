// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SEEN_AI_NATIVE_FOREIGN_H
#define SEEN_AI_NATIVE_FOREIGN_H

#include <string>
class SPDocument;
namespace Inkscape::XML { class Node; }
namespace Inkscape::Extension::Internal::AiNative {
struct ForeignObject;
/// Import the bounded vector subset of an embedded foreign PDF into a detached group.
/// Unsupported PDF operators/resources refuse the whole object, including tiny ones.
XML::Node *import_foreign_pdf(ForeignObject const &object, SPDocument *doc, std::string &error);
}
#endif
