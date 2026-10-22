// SPDX-License-Identifier: GPL-2.0-or-later
/** @file
 * PDF parsing using libpoppler.
 *//*
 * Authors:
 * Derived from poppler's Gfx.cc, which was derived from Xpdf by 1996-2003 Glyph & Cog, LLC
 * Jon A. Cruz <jon@joncruz.org>
 *
 * Copyright (C) 2018 Authors
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"  // only include where actually required!
#endif

#include "pdf-parser.h"

#ifdef USE_GCC_PRAGMAS
#pragma implementation
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex> // std::call_once()
#include <utility>
#include <vector>
#include <2geom/transforms.h>

#include <Annot.h>
#include <Array.h>
#include <CharTypes.h>
#include <Dict.h>
#include <Error.h>
#include <Gfx.h>
#include <GfxFont.h>
#include <GfxState.h>
#include <GlobalParams.h>
#include <Lexer.h>
#include <Object.h>
#include <OptionalContent.h>
#include <OutputDev.h>
#include <PDFDoc.h>
#include <Page.h>
#include <Parser.h>
#include <Stream.h>
#include <glib/poppler-features.h>
#include <goo/GooString.h>
#include <goo/gmem.h>
#include "pdf-utils.h"
#include "poppler-cairo-font-engine.h"
#include "poppler-transition-api.h"
#include "poppler-utils.h"
#include "svg-builder.h"

// the MSVC math.h doesn't define this
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

//------------------------------------------------------------------------
// constants
//------------------------------------------------------------------------

// Default max delta allowed in any color component for a shading fill.
#define defaultShadingColorDelta (dblToCol( 1 / 2.0 ))

// Default max recursive depth for a shading fill.
#define defaultShadingMaxDepth 6

// Max number of operators kept in the history list.
#define maxOperatorHistoryDepth 16

//------------------------------------------------------------------------
// Operator table
//------------------------------------------------------------------------

PdfOperator PdfParser::opTab[] = {
  {"\"",  3, {tchkNum,    tchkNum,    tchkString},
          &PdfParser::opMoveSetShowText},
  {"'",   1, {tchkString},
          &PdfParser::opMoveShowText},
  {"B",   0, {tchkNone},
          &PdfParser::opFillStroke},
  {"B*",  0, {tchkNone},
          &PdfParser::opEOFillStroke},
  {"BDC", 2, {tchkName,   tchkProps},
          &PdfParser::opBeginMarkedContent},
  {"BI",  0, {tchkNone},
          &PdfParser::opBeginImage},
  {"BMC", 1, {tchkName},
          &PdfParser::opBeginMarkedContent},
  {"BT",  0, {tchkNone},
          &PdfParser::opBeginText},
  {"BX",  0, {tchkNone},
          &PdfParser::opBeginIgnoreUndef},
  {"CS",  1, {tchkName},
          &PdfParser::opSetStrokeColorSpace},
  {"DP",  2, {tchkName,   tchkProps},
          &PdfParser::opMarkPoint},
  {"Do",  1, {tchkName},
          &PdfParser::opXObject},
  {"EI",  0, {tchkNone},
          &PdfParser::opEndImage},
  {"EMC", 0, {tchkNone},
          &PdfParser::opEndMarkedContent},
  {"ET",  0, {tchkNone},
          &PdfParser::opEndText},
  {"EX",  0, {tchkNone},
          &PdfParser::opEndIgnoreUndef},
  {"F",   0, {tchkNone},
          &PdfParser::opFill},
  {"G",   1, {tchkNum},
          &PdfParser::opSetStrokeGray},
  {"ID",  0, {tchkNone},
          &PdfParser::opImageData},
  {"J",   1, {tchkInt},
          &PdfParser::opSetLineCap},
  {"K",   4, {tchkNum,    tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetStrokeCMYKColor},
  {"M",   1, {tchkNum},
          &PdfParser::opSetMiterLimit},
  {"MP",  1, {tchkName},
          &PdfParser::opMarkPoint},
  {"Q",   0, {tchkNone},
          &PdfParser::opRestore},
  {"RG",  3, {tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetStrokeRGBColor},
  {"S",   0, {tchkNone},
          &PdfParser::opStroke},
  {"SC",  -4, {tchkNum,   tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetStrokeColor},
  {"SCN", -33, {tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,},
          &PdfParser::opSetStrokeColorN},
  {"T*",  0, {tchkNone},
          &PdfParser::opTextNextLine},
  {"TD",  2, {tchkNum,    tchkNum},
          &PdfParser::opTextMoveSet},
  {"TJ",  1, {tchkArray},
          &PdfParser::opShowSpaceText},
  {"TL",  1, {tchkNum},
          &PdfParser::opSetTextLeading},
  {"Tc",  1, {tchkNum},
          &PdfParser::opSetCharSpacing},
  {"Td",  2, {tchkNum,    tchkNum},
          &PdfParser::opTextMove},
  {"Tf",  2, {tchkName,   tchkNum},
          &PdfParser::opSetFont},
  {"Tj",  1, {tchkString},
          &PdfParser::opShowText},
  {"Tm",  6, {tchkNum,    tchkNum,    tchkNum,    tchkNum,
	      tchkNum,    tchkNum},
          &PdfParser::opSetTextMatrix},
  {"Tr",  1, {tchkInt},
          &PdfParser::opSetTextRender},
  {"Ts",  1, {tchkNum},
          &PdfParser::opSetTextRise},
  {"Tw",  1, {tchkNum},
          &PdfParser::opSetWordSpacing},
  {"Tz",  1, {tchkNum},
          &PdfParser::opSetHorizScaling},
  {"W",   0, {tchkNone},
          &PdfParser::opClip},
  {"W*",  0, {tchkNone},
          &PdfParser::opEOClip},
  {"b",   0, {tchkNone},
          &PdfParser::opCloseFillStroke},
  {"b*",  0, {tchkNone},
          &PdfParser::opCloseEOFillStroke},
  {"c",   6, {tchkNum,    tchkNum,    tchkNum,    tchkNum,
	      tchkNum,    tchkNum},
          &PdfParser::opCurveTo},
  {"cm",  6, {tchkNum,    tchkNum,    tchkNum,    tchkNum,
	      tchkNum,    tchkNum},
          &PdfParser::opConcat},
  {"cs",  1, {tchkName},
          &PdfParser::opSetFillColorSpace},
  {"d",   2, {tchkArray,  tchkNum},
          &PdfParser::opSetDash},
  {"d0",  2, {tchkNum,    tchkNum},
          &PdfParser::opSetCharWidth},
  {"d1",  6, {tchkNum,    tchkNum,    tchkNum,    tchkNum,
	      tchkNum,    tchkNum},
          &PdfParser::opSetCacheDevice},
  {"f",   0, {tchkNone},
          &PdfParser::opFill},
  {"f*",  0, {tchkNone},
          &PdfParser::opEOFill},
  {"g",   1, {tchkNum},
          &PdfParser::opSetFillGray},
  {"gs",  1, {tchkName},
          &PdfParser::opSetExtGState},
  {"h",   0, {tchkNone},
          &PdfParser::opClosePath},
  {"i",   1, {tchkNum},
          &PdfParser::opSetFlat},
  {"j",   1, {tchkInt},
          &PdfParser::opSetLineJoin},
  {"k",   4, {tchkNum,    tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetFillCMYKColor},
  {"l",   2, {tchkNum,    tchkNum},
          &PdfParser::opLineTo},
  {"m",   2, {tchkNum,    tchkNum},
          &PdfParser::opMoveTo},
  {"n",   0, {tchkNone},
          &PdfParser::opEndPath},
  {"q",   0, {tchkNone},
          &PdfParser::opSave},
  {"re",  4, {tchkNum,    tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opRectangle},
  {"rg",  3, {tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetFillRGBColor},
  {"ri",  1, {tchkName},
          &PdfParser::opSetRenderingIntent},
  {"s",   0, {tchkNone},
          &PdfParser::opCloseStroke},
  {"sc",  -4, {tchkNum,   tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opSetFillColor},
  {"scn", -33, {tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,   tchkSCN,    tchkSCN,    tchkSCN,
	        tchkSCN,},
          &PdfParser::opSetFillColorN},
  {"sh",  1, {tchkName},
          &PdfParser::opShFill},
  {"v",   4, {tchkNum,    tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opCurveTo1},
  {"w",   1, {tchkNum},
          &PdfParser::opSetLineWidth},
  {"y",   4, {tchkNum,    tchkNum,    tchkNum,    tchkNum},
          &PdfParser::opCurveTo2}
};

#define numOps (sizeof(opTab) / sizeof(PdfOperator))

namespace {

GfxPatch blankPatch()
{
    GfxPatch patch;
    memset(&patch, 0, sizeof(patch)); // quick-n-dirty
    return patch;
}

} // namespace

//------------------------------------------------------------------------
// PdfParser
//------------------------------------------------------------------------

PdfParser::PdfParser(std::shared_ptr<PDFDoc> pdf_doc, Inkscape::Extension::Internal::SvgBuilder *builderA, Page *page,
                     const std::optional<PDFRectangle> &cropBox)
    : _pdf_doc(pdf_doc)
    , xref(pdf_doc->getXRef())
    , builder(builderA)
    , subPage(false)
    , printCommands(false)
    , res(new GfxResources(xref, page->getResourceDict(), nullptr))
    , // start the resource stack
    state(new GfxState(96.0, 96.0, page->getCropBox(), page->getRotate(), true))
    , fontChanged(gFalse)
    , clip(clipNone)
    , ignoreUndef(0)
    , formDepth(0)
    , parser(nullptr)
    , operatorHistory(nullptr)
{
    setDefaultApproximationPrecision();
    loadOptionalContentLayers(page->getResourceDict());
    loadColorProfile();
    baseMatrix = stateToAffine(state);
    builder->setBaseMatrix(baseMatrix);

    if (page) {
        // Increment the page building here and set page label
        Catalog *catalog = pdf_doc->getCatalog();
        _POPPLER_STRING_26_7 label;
        catalog->indexToLabel(page->getNum() - 1, &label);
        builder->pushPage(getString(label), state);
    }

    // Must come after pushPage!
    builder->setDocumentSize(state->getPageWidth(), state->getPageHeight());

    // Set margins, bleeds and page-cropping
    auto page_box = getRect(page->getCropBox());
    auto scale = Geom::Scale(state->getPageWidth() / page_box.width(),
                             state->getPageHeight() / page_box.height());
    builder->setMargins(getRect(page->getTrimBox()) * scale,
                        getRect(page->getArtBox()) * scale,
                        getRect(page->getMediaBox()) * scale);
    if (cropBox && getRect(*cropBox) != page_box) {
        builder->cropPage(getRect(*cropBox) * scale);
    }

    if (auto meta = pdf_doc->readMetadata()) {
        // TODO: Parse this metadat RDF document and extract SVG RDF details from it.
        // meta->getCString()
    }

    builder->setMetadata("title", getString(pdf_doc->getDocInfoStringEntry("Title")));
    builder->setMetadata("description", getString(pdf_doc->getDocInfoStringEntry("Subject")));
    builder->setMetadata("creator", getString(pdf_doc->getDocInfoStringEntry("Author")));
    builder->setMetadata("subject", getString(pdf_doc->getDocInfoStringEntry("Keywords")));
    builder->setMetadata("date", getString(pdf_doc->getDocInfoStringEntry("CreationDate")));

    formDepth = 0;

    pushOperator("startPage");
}

PdfParser::PdfParser(XRef *xrefA, Inkscape::Extension::Internal::SvgBuilder *builderA, Dict *resDict,
                     _POPPLER_CONST PDFRectangle *box)
    : xref(xrefA)
    , builder(builderA)
    , subPage(true)
    , printCommands(false)
    , res(new GfxResources(xref, resDict, nullptr))
    , // start the resource stack
    state(new _POPPLER_GFX_STATE(72, 72, *box, 0, false))
    , fontChanged(gFalse)
    , clip(clipNone)
    , ignoreUndef(0)
    , formDepth(0)
    , parser(nullptr)
    , operatorHistory(nullptr)
{
    setDefaultApproximationPrecision();
    baseMatrix = stateToAffine(state);
    builder->setBaseMatrix(baseMatrix);
    formDepth = 0;
}

PdfParser::~PdfParser() {
  while(operatorHistory) {
    OpHistoryEntry *tmp = operatorHistory->next;
    delete operatorHistory;
    operatorHistory = tmp;
  }

  while (state && state->hasSaves()) {
    restoreState();
  }

  if (!subPage) {
    //out->endPage();
  }

  while (res) {
    popResources();
  }

  if (state) {
    delete state;
    state = nullptr;
  }
}

void PdfParser::parse(Object *obj, GBool topLevel) {
  Object obj2;

  if (obj->isArray()) {
    for (int i = 0; i < obj->getArray()->getLength(); ++i) {
      _POPPLER_CALL_ARGS(obj2, obj->getArray()->get, i);
      if (!obj2.isStream()) {
	error(errInternal, -1, "Weird page contents");
	_POPPLER_FREE(obj2);
	return;
      }
      _POPPLER_FREE(obj2);
    }
  } else if (!obj->isStream()) {
	error(errInternal, -1, "Weird page contents");
    	return;
  }
  parser = new _POPPLER_NEW_PARSER(xref, obj);
  go(topLevel);
  delete parser;
  parser = nullptr;
}

void PdfParser::go(GBool /*topLevel*/)
{
  Object obj;
  Object args[maxArgs];

  // scan a sequence of objects
  int numArgs = 0;
  _POPPLER_CALL(obj, parser->getObj);
  while (!obj.isEOF()) {

    // got a command - execute it
    if (obj.isCmd()) {
      if (printCommands) {
	obj.print(stdout);
	for (int i = 0; i < numArgs; ++i) {
	  printf(" ");
	  args[i].print(stdout);
	}
	printf("\n");
	fflush(stdout);
      }

      // Run the operation
      execOp(&obj, args, numArgs);

#if !defined(POPPLER_NEW_OBJECT_API)
      _POPPLER_FREE(obj);
      for (int i = 0; i < numArgs; ++i)
	_POPPLER_FREE(args[i]);
#endif
      numArgs = 0;

    // got an argument - save it
    } else if (numArgs < maxArgs) {
      args[numArgs++] = std::move(obj);

    // too many arguments - something is wrong
    } else {
      error(errSyntaxError, getPos(), "Too many args in content stream");
      if (printCommands) {
	printf("throwing away arg: ");
	obj.print(stdout);
	printf("\n");
	fflush(stdout);
      }
      _POPPLER_FREE(obj);
    }

    // grab the next object
    _POPPLER_CALL(obj, parser->getObj);
  }
  _POPPLER_FREE(obj);

  // args at end with no command
  if (numArgs > 0) {
    error(errSyntaxError, getPos(), "Leftover args in content stream");
    if (printCommands) {
      printf("%d leftovers:", numArgs);
      for (int i = 0; i < numArgs; ++i) {
	printf(" ");
	args[i].print(stdout);
      }
      printf("\n");
      fflush(stdout);
    }
#if !defined(POPPLER_NEW_OBJECT_API)
    for (int i = 0; i < numArgs; ++i)
      _POPPLER_FREE(args[i]);
#endif
  }
}

void PdfParser::pushOperator(const char *name)
{
    OpHistoryEntry *newEntry = new OpHistoryEntry;
    newEntry->name = name;
    newEntry->state = nullptr;
    newEntry->depth = (operatorHistory != nullptr ? (operatorHistory->depth+1) : 0);
    newEntry->next = operatorHistory;
    operatorHistory = newEntry;

    // Truncate list if needed
    if (operatorHistory->depth > maxOperatorHistoryDepth) {
        OpHistoryEntry *curr = operatorHistory;
        OpHistoryEntry *prev = nullptr;
        while (curr && curr->next != nullptr) {
            curr->depth--;
            prev = curr;
            curr = curr->next;
        }
        if (prev) {
            if (curr->state != nullptr)
                delete curr->state;
            delete curr;
            prev->next = nullptr;
        }
    }
}

const char *PdfParser::getPreviousOperator(unsigned int look_back) {
    OpHistoryEntry *prev = nullptr;
    if (operatorHistory != nullptr && look_back > 0) {
        prev = operatorHistory->next;
        while (--look_back > 0 && prev != nullptr) {
            prev = prev->next;
        }
    }
    if (prev != nullptr) {
        return prev->name;
    } else {
        return "";
    }
}

void PdfParser::execOp(Object *cmd, Object args[], int numArgs) {
  PdfOperator *op;
  const char *name;
  Object *argPtr;
  int i;

  // find operator
  name = cmd->getCmd();
    if (!(op = findOp(name))) {
    if (ignoreUndef == 0)
      error(errSyntaxError, getPos(), "Unknown operator '{0:s}'", name);
    return;
  }

  // type check args
  argPtr = args;
  if (op->numArgs >= 0) {
    if (numArgs < op->numArgs) {
      error(errSyntaxError, getPos(), "Too few ({0:d}) args to '{1:d}' operator", numArgs, name);
      return;
    }
    if (numArgs > op->numArgs) {
#if 0
      error(errSyntaxError, getPos(), "Too many ({0:d}) args to '{1:s}' operator", numArgs, name);
#endif
      argPtr += numArgs - op->numArgs;
      numArgs = op->numArgs;
    }
  } else {
    if (numArgs > -op->numArgs) {
      error(errSyntaxError, getPos(), "Too many ({0:d}) args to '{1:s}' operator",
	    numArgs, name);
      return;
    }
  }
  for (i = 0; i < numArgs; ++i) {
    if (!checkArg(&argPtr[i], op->tchk[i])) {
      error(errSyntaxError, getPos(), "Arg #{0:d} to '{1:s}' operator is wrong type ({2:s})",
	    i, name, argPtr[i].getTypeName());
      return;
    }
  }

  // add to history
  pushOperator((char*)&op->name);

  // do it
  (this->*op->func)(argPtr, numArgs);
}

PdfOperator* PdfParser::findOp(const char *name) {
  int a = -1;
  int b = numOps;
  int cmp = -1;
  // invariant: opTab[a] < name < opTab[b]
  while (b - a > 1) {
    const int m = (a + b) / 2;
    cmp = strcmp(opTab[m].name, name);
    if (cmp < 0)
      a = m;
    else if (cmp > 0)
      b = m;
    else
      a = b = m;
  }
  if (cmp != 0) {
      return nullptr;
  }
  return &opTab[a];
}

GBool PdfParser::checkArg(Object *arg, TchkType type) {
  switch (type) {
  case tchkBool:   return arg->isBool();
  case tchkInt:    return arg->isInt();
  case tchkNum:    return arg->isNum();
  case tchkString: return arg->isString();
  case tchkName:   return arg->isName();
  case tchkArray:  return arg->isArray();
  case tchkProps:  return arg->isDict() || arg->isName();
  case tchkSCN:    return arg->isNum() || arg->isName();
  case tchkNone:   return gFalse;
  }
  return gFalse;
}

int PdfParser::getPos() {
  return parser ? parser->getPos() : -1;
}

//------------------------------------------------------------------------
// graphics state operators
//------------------------------------------------------------------------

void PdfParser::opSave(Object /*args*/[], int /*numArgs*/)
{
  saveState();
}

void PdfParser::opRestore(Object /*args*/[], int /*numArgs*/)
{
  restoreState();
}

// TODO not good that numArgs is ignored but args[] is used:
/**
 * Concatenate transformation matrix to the current state
 */
void PdfParser::opConcat(Object args[], int /*numArgs*/)
{
  state->concatCTM(args[0].getNum(), args[1].getNum(),
		   args[2].getNum(), args[3].getNum(),
		   args[4].getNum(), args[5].getNum());
  fontChanged = gTrue;
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetDash(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);

  double *dash = nullptr;

  Array *a = args[0].getArray();
  int length = a->getLength();
  if (length != 0) {
    dash = (double *)gmallocn(length, sizeof(double));
    for (int i = 0; i < length; ++i) {
      Object obj;
      dash[i] = _POPPLER_CALL_ARGS_DEREF(obj, a->get, i).getNum();
      _POPPLER_FREE(obj);
    }
  }
#if POPPLER_CHECK_VERSION(22, 9, 0)
  state->setLineDash(std::vector<double> (dash, dash + length), args[1].getNum());
#else
  state->setLineDash(dash, length, args[1].getNum());
#endif
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFlat(Object args[], int /*numArgs*/)
{
  state->setFlatness((int)args[0].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetLineJoin(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);
#if POPPLER_CHECK_VERSION(26,2,0)
  state->setLineJoin((GfxState::LineJoinStyle) args[0].getInt());
#else
  state->setLineJoin(args[0].getInt());
#endif
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetLineCap(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);
#if POPPLER_CHECK_VERSION(26,2,0)
  state->setLineCap((GfxState::LineCapStyle) args[0].getInt());
#else
  state->setLineCap(args[0].getInt());
#endif
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetMiterLimit(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);
  state->setMiterLimit(args[0].getNum());
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetLineWidth(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);
  state->setLineWidth(args[0].getNum());
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetExtGState(Object args[], int /*numArgs*/)
{
    Object obj1, obj2, obj3, obj4, obj5;
    GfxColor backdropColor;
    GBool haveBackdropColor = gFalse;
    GBool alpha = gFalse;

    _POPPLER_CALL_ARGS(obj1, res->lookupGState, args[0].getName());
    if (obj1.isNull()) {
        return;
    }
    if (!obj1.isDict()) {
        error(errSyntaxError, getPos(), "ExtGState '{0:s}' is wrong type"), args[0].getName();
        _POPPLER_FREE(obj1);
        return;
    }
    if (printCommands) {
        printf("  gfx state dict: ");
        obj1.print();
        printf("\n");
    }

    // transparency support: blend mode, fill/stroke opacity
    if (!_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "BM").isNull()) {
        GfxBlendMode mode = gfxBlendNormal;
        if (state->parseBlendMode(&obj2, &mode)) {
            state->setBlendMode(mode);
        } else {
            error(errSyntaxError, getPos(), "Invalid blend mode in ExtGState");
        }
    }
    if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "ca").isNum()) {
        state->setFillOpacity(obj2.getNum());
    }
    if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "CA").isNum()) {
        state->setStrokeOpacity(obj2.getNum());
    }

    // fill/stroke overprint
    GBool haveFillOP = gFalse;
    if ((haveFillOP = _POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "op").isBool())) {
        state->setFillOverprint(obj2.getBool());
    }
    if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "OP").isBool()) {
        state->setStrokeOverprint(obj2.getBool());
        if (!haveFillOP) {
            state->setFillOverprint(obj2.getBool());
        }
    }

    // stroke adjust
    if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "SA").isBool()) {
        state->setStrokeAdjust(obj2.getBool());
    }

    // Note: Transfer functions in graphics state are ignored for SVG conversion
    // See https://gitlab.com/inkscape/inkscape/-/merge_requests/7690 for discussion.

    // Stroke width
    if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "LW").isNum()) {
        state->setLineWidth(obj2.getNum());
    }

    // soft mask
    if (!_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "SMask").isNull()) {
        if (obj2.isName(const_cast<char *>("None"))) {
            // Do nothing.
        } else if (obj2.isDict()) {
            if (_POPPLER_CALL_ARGS_DEREF(obj3, obj2.dictLookup, "S").isName("Alpha")) {
                alpha = gTrue;
            } else { // "Luminosity"
                alpha = gFalse;
            }
            _POPPLER_FREE(obj3);
            _POPPLER_DECLARE_TRANSFER_FUNCTION(softMaskTransferFunc);
            if (!_POPPLER_CALL_ARGS_DEREF(obj3, obj2.dictLookup, "TR").isNull()) {
                softMaskTransferFunc = Function::parse(&obj3);
                if (softMaskTransferFunc->getInputSize() != 1 || softMaskTransferFunc->getOutputSize() != 1) {
                    error(errSyntaxError, getPos(), "Invalid transfer function in soft mask in ExtGState");
                    _POPPLER_DELETE_TRANSFER_FUNCTION(softMaskTransferFunc);
                }
            }
            _POPPLER_FREE(obj3);
            if ((haveBackdropColor = _POPPLER_CALL_ARGS_DEREF(obj3, obj2.dictLookup, "BC").isArray())) {
                for (int &i : backdropColor.c) {
                    i = 0;
                }
                for (int i = 0; i < obj3.getArray()->getLength() && i < gfxColorMaxComps; ++i) {
                    _POPPLER_CALL_ARGS(obj4, obj3.getArray()->get, i);
                    if (obj4.isNum()) {
                        backdropColor.c[i] = dblToCol(obj4.getNum());
                    }
                    _POPPLER_FREE(obj4);
                }
            }
            _POPPLER_FREE(obj3);
            if (_POPPLER_CALL_ARGS_DEREF(obj3, obj2.dictLookup, "G").isStream()) {
                if (_POPPLER_CALL_ARGS_DEREF(obj4, obj3.getStream()->getDict()->lookup, "Group").isDict()) {
                    std::unique_ptr<GfxColorSpace> blendingColorSpace;
                    GBool isolated = gFalse;
                    GBool knockout = gFalse;
                    if (!_POPPLER_CALL_ARGS_DEREF(obj5, obj4.dictLookup, "CS").isNull()) {
                        blendingColorSpace = std::unique_ptr<GfxColorSpace>(GfxColorSpace::parse(nullptr, &obj5, nullptr, state));
                    }
                    _POPPLER_FREE(obj5);
                    if (_POPPLER_CALL_ARGS_DEREF(obj5, obj4.dictLookup, "I").isBool()) {
                        isolated = obj5.getBool();
                    }
                    _POPPLER_FREE(obj5);
                    if (_POPPLER_CALL_ARGS_DEREF(obj5, obj4.dictLookup, "K").isBool()) {
                        knockout = obj5.getBool();
                    }
                    _POPPLER_FREE(obj5);
                    if (!haveBackdropColor) {
                        if (blendingColorSpace) {
                            blendingColorSpace->getDefaultColor(&backdropColor);
                        } else {
                            //~ need to get the parent or default color space (?)
                            for (int &i : backdropColor.c) {
                                i = 0;
                            }
                        }
                    }
                    doSoftMask(&obj3, alpha, blendingColorSpace.get(), isolated, knockout,
                               _POPPLER_GET_TRANSFER_FUNCTION_POINTER(softMaskTransferFunc), &backdropColor);
                    if (softMaskTransferFunc) {
                        _POPPLER_DELETE_TRANSFER_FUNCTION(softMaskTransferFunc);
                    }
                } else {
                    error(errSyntaxError, getPos(), "Invalid soft mask in ExtGState - missing group");
                }
                _POPPLER_FREE(obj4);
            } else {
                error(errSyntaxError, getPos(), "Invalid soft mask in ExtGState - missing group");
            }
            _POPPLER_FREE(obj3);
        } else if (!obj2.isNull()) {
            error(errSyntaxError, getPos(), "Invalid soft mask in ExtGState");
        }
    }

    _POPPLER_FREE(obj2);
    _POPPLER_FREE(obj1);
}

void PdfParser::doSoftMask(Object *str, GBool alpha,
		     GfxColorSpace *blendingColorSpace,
		     GBool isolated, GBool knockout,
		     Function *transferFunc, GfxColor *backdropColor) {
  Dict *dict, *resDict;
  double m[6], bbox[4];
  Object obj1, obj2;
  int i;

  // check for excessive recursion
  if (formDepth > 20) {
    return;
  }

  // get stream dict
  dict = str->getStream()->getDict();

  // check form type
  _POPPLER_CALL_ARGS(obj1, dict->lookup, "FormType");
  if (!(obj1.isNull() || (obj1.isInt() && obj1.getInt() == 1))) {
    error(errSyntaxError, getPos(), "Unknown form type");
  }
  _POPPLER_FREE(obj1);

  // get bounding box
  _POPPLER_CALL_ARGS(obj1, dict->lookup, "BBox");
  if (!obj1.isArray()) {
    _POPPLER_FREE(obj1);
    error(errSyntaxError, getPos(), "Bad form bounding box");
    return;
  }
  for (i = 0; i < 4; ++i) {
    _POPPLER_CALL_ARGS(obj2, obj1.getArray()->get, i);
    bbox[i] = obj2.getNum();
    _POPPLER_FREE(obj2);
  }
  _POPPLER_FREE(obj1);

  // get matrix
  _POPPLER_CALL_ARGS(obj1, dict->lookup, "Matrix");
  if (obj1.isArray()) {
    for (i = 0; i < 6; ++i) {
      _POPPLER_CALL_ARGS(obj2, obj1.getArray()->get, i);
      m[i] = obj2.getNum();
      _POPPLER_FREE(obj2);
    }
  } else {
    m[0] = 1; m[1] = 0;
    m[2] = 0; m[3] = 1;
    m[4] = 0; m[5] = 0;
  }
  _POPPLER_FREE(obj1);

  // get resources
  _POPPLER_CALL_ARGS(obj1, dict->lookup, "Resources");
  resDict = obj1.isDict() ? obj1.getDict() : (Dict *)nullptr;

  // draw it
  doForm1(str, resDict, m, bbox, gTrue, gTrue,
	  blendingColorSpace, isolated, knockout,
	  alpha, transferFunc, backdropColor);

  _POPPLER_FREE(obj1);
}

void PdfParser::opSetRenderingIntent(Object /*args*/[], int /*numArgs*/)
{
}

//------------------------------------------------------------------------
// color operators
//------------------------------------------------------------------------

/**
 * Get a newly allocated color space instance by CS operation argument.
 *
 * Maintains a cache for named color spaces to avoid expensive re-parsing.
 */
std::unique_ptr<GfxColorSpace> PdfParser::lookupColorSpaceCopy(Object &arg)
{
    assert(!arg.isNull());

    if (char const *name = arg.isName() ? arg.getName() : nullptr) {
        auto const cache_name = std::to_string(formDepth) + "-" + name;
        if (auto cached = colorSpacesCache[cache_name].get()) {
            return std::unique_ptr<GfxColorSpace>(cached->copy());
        }

        std::unique_ptr<GfxColorSpace> colorSpace;
        if (auto obj = res->lookupColorSpace(name); !obj.isNull()) {
            colorSpace = std::unique_ptr<GfxColorSpace>(GfxColorSpace::parse(res, &obj, nullptr, state));
        } else {
            colorSpace = std::unique_ptr<GfxColorSpace>(GfxColorSpace::parse(res, &arg, nullptr, state));
        }

        if (colorSpace && colorSpace->getMode() != csPattern) {
            colorSpacesCache[cache_name] = std::unique_ptr<GfxColorSpace>(colorSpace->copy());
        }

        return colorSpace;
    } else {
        // We were passed in an object directly.
        return std::unique_ptr<GfxColorSpace>(GfxColorSpace::parse(res, &arg, nullptr, state));
    }
}

/**
 * Look up pattern/gradients from the GfxResource dictionary
 */
std::unique_ptr<GfxPattern> PdfParser::lookupPattern(Object *obj, GfxState *state)
{
    if (!obj->isName()) {
        return {};
    }
    return std::unique_ptr<GfxPattern>(res->lookupPattern(obj->getName(), nullptr, state));
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFillGray(Object args[], int /*numArgs*/)
{
  GfxColor color;
  builder->beforeStateChange(state);
  state->setFillPattern(nullptr);
  state->setFillColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceGrayColorSpace>()));
  color.c[0] = dblToCol(args[0].getNum());
  state->_POPPLER_SET_FILL_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetStrokeGray(Object args[], int /*numArgs*/)
{
  GfxColor color;
  builder->beforeStateChange(state);
  state->setStrokePattern(nullptr);
  state->setStrokeColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceGrayColorSpace>()));
  color.c[0] = dblToCol(args[0].getNum());
  state->_POPPLER_SET_STROKE_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFillCMYKColor(Object args[], int /*numArgs*/)
{
  GfxColor color;
  int i;
  builder->beforeStateChange(state);
  state->setFillPattern(nullptr);
  state->setFillColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceCMYKColorSpace>()));
  for (i = 0; i < 4; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_FILL_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetStrokeCMYKColor(Object args[], int /*numArgs*/)
{
  GfxColor color;
  builder->beforeStateChange(state);
  state->setStrokePattern(nullptr);
  state->setStrokeColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceCMYKColorSpace>()));
  for (int i = 0; i < 4; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_STROKE_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFillRGBColor(Object args[], int /*numArgs*/)
{
  GfxColor color;
  builder->beforeStateChange(state);
  state->setFillPattern(nullptr);
  state->setFillColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceRGBColorSpace>()));
  for (int i = 0; i < 3; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_FILL_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetStrokeRGBColor(Object args[], int /*numArgs*/) {
  GfxColor color;
  builder->beforeStateChange(state);
  state->setStrokePattern(nullptr);
  state->setStrokeColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(std::make_unique<GfxDeviceRGBColorSpace>()));
  for (int i = 0; i < 3; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_STROKE_COLOR(color);
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFillColorSpace(Object args[], int numArgs)
{
  assert(numArgs >= 1);
  auto colorSpace = lookupColorSpaceCopy(args[0]);
  builder->beforeStateChange(state);
  state->setFillPattern(nullptr);

  if (colorSpace) {
    GfxColor color;
    colorSpace->getDefaultColor(&color);
    state->setFillColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(colorSpace));
    state->_POPPLER_SET_FILL_COLOR(color);
    builder->updateStyle(state);
  } else {
    error(errSyntaxError, getPos(), "Bad color space (fill)");
  }
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetStrokeColorSpace(Object args[], int numArgs)
{
  assert(numArgs >= 1);
  builder->beforeStateChange(state);

  auto colorSpace = lookupColorSpaceCopy(args[0]);

  state->setStrokePattern(nullptr);

  if (colorSpace) {
    GfxColor color;
    colorSpace->getDefaultColor(&color);
    state->setStrokeColorSpace(_POPPLER_CONSUME_UNIQPTR_ARG(colorSpace));
    state->_POPPLER_SET_STROKE_COLOR(color);
    builder->updateStyle(state);
  } else {
    error(errSyntaxError, getPos(), "Bad color space (stroke)");
  }
}

void PdfParser::opSetFillColor(Object args[], int numArgs) {
  GfxColor color;
  int i;

  if (numArgs != state->getFillColorSpace()->getNComps()) {
    error(errSyntaxError, getPos(), "Incorrect number of arguments in 'sc' command");
    return;
  }
  builder->beforeStateChange(state);
  state->setFillPattern(nullptr);
  for (i = 0; i < numArgs; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_FILL_COLOR(color);
  builder->updateStyle(state);
}

void PdfParser::opSetStrokeColor(Object args[], int numArgs) {
  GfxColor color;
  int i;

  if (numArgs != state->getStrokeColorSpace()->getNComps()) {
    error(errSyntaxError, getPos(), "Incorrect number of arguments in 'SC' command");
    return;
  }
  builder->beforeStateChange(state);
  state->setStrokePattern(nullptr);
  for (i = 0; i < numArgs; ++i) {
    color.c[i] = dblToCol(args[i].getNum());
  }
  state->_POPPLER_SET_STROKE_COLOR(color);
  builder->updateStyle(state);
}

void PdfParser::opSetFillColorN(Object args[], int numArgs) {
  GfxColor color;
  int i;
  builder->beforeStateChange(state);
  if (state->getFillColorSpace()->getMode() == csPattern) {
    if (numArgs > 1) {
      if (!((GfxPatternColorSpace *)state->getFillColorSpace())->getUnder() ||
	  numArgs - 1 != ((GfxPatternColorSpace *)state->getFillColorSpace())
	                     ->getUnder()->getNComps()) {
	error(errSyntaxError, getPos(), "Incorrect number of arguments in 'scn' command");
	return;
      }
      for (i = 0; i < numArgs - 1 && i < gfxColorMaxComps; ++i) {
	if (args[i].isNum()) {
	  color.c[i] = dblToCol(args[i].getNum());
	}
      }
      state->_POPPLER_SET_FILL_COLOR(color);
      builder->updateStyle(state);
    }
    if (auto pattern = lookupPattern(&(args[numArgs - 1]), state)) {
        state->setFillPattern(_POPPLER_CONSUME_UNIQPTR_ARG(pattern));
        builder->updateStyle(state);
    }

  } else {
    if (numArgs != state->getFillColorSpace()->getNComps()) {
      error(errSyntaxError, getPos(), "Incorrect number of arguments in 'scn' command");
      return;
    }
    state->setFillPattern(nullptr);
    for (i = 0; i < numArgs && i < gfxColorMaxComps; ++i) {
      if (args[i].isNum()) {
	color.c[i] = dblToCol(args[i].getNum());
      }
    }
    state->_POPPLER_SET_FILL_COLOR(color);
    builder->updateStyle(state);
  }
}

void PdfParser::opSetStrokeColorN(Object args[], int numArgs) {
  GfxColor color;
  int i;
  builder->beforeStateChange(state);

  if (state->getStrokeColorSpace()->getMode() == csPattern) {
    if (numArgs > 1) {
      if (!((GfxPatternColorSpace *)state->getStrokeColorSpace())
	       ->getUnder() ||
	  numArgs - 1 != ((GfxPatternColorSpace *)state->getStrokeColorSpace())
	                     ->getUnder()->getNComps()) {
	error(errSyntaxError, getPos(), "Incorrect number of arguments in 'SCN' command");
	return;
      }
      for (i = 0; i < numArgs - 1 && i < gfxColorMaxComps; ++i) {
	if (args[i].isNum()) {
	  color.c[i] = dblToCol(args[i].getNum());
	}
      }
      state->_POPPLER_SET_STROKE_COLOR(color);
      builder->updateStyle(state);
    }
    if (auto pattern = lookupPattern(&(args[numArgs - 1]), state)) {
        state->setStrokePattern(_POPPLER_CONSUME_UNIQPTR_ARG(pattern));
        builder->updateStyle(state);
    }

  } else {
    if (numArgs != state->getStrokeColorSpace()->getNComps()) {
      error(errSyntaxError, getPos(), "Incorrect number of arguments in 'SCN' command");
      return;
    }
    state->setStrokePattern(nullptr);
    for (i = 0; i < numArgs && i < gfxColorMaxComps; ++i) {
      if (args[i].isNum()) {
	color.c[i] = dblToCol(args[i].getNum());
      }
    }
    state->_POPPLER_SET_STROKE_COLOR(color);
    builder->updateStyle(state);
  }
}

//------------------------------------------------------------------------
// path segment operators
//------------------------------------------------------------------------

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opMoveTo(Object args[], int /*numArgs*/)
{
  state->moveTo(args[0].getNum(), args[1].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opLineTo(Object args[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    error(errSyntaxError, getPos(), "No current point in lineto");
    return;
  }
  state->lineTo(args[0].getNum(), args[1].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opCurveTo(Object args[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    error(errSyntaxError, getPos(), "No current point in curveto");
    return;
  }
  double x1 = args[0].getNum();
  double y1 = args[1].getNum();
  double x2 = args[2].getNum();
  double y2 = args[3].getNum();
  double x3 = args[4].getNum();
  double y3 = args[5].getNum();
  state->curveTo(x1, y1, x2, y2, x3, y3);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opCurveTo1(Object args[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    error(errSyntaxError, getPos(), "No current point in curveto1");
    return;
  }
  double x1 = state->getCurX();
  double y1 = state->getCurY();
  double x2 = args[0].getNum();
  double y2 = args[1].getNum();
  double x3 = args[2].getNum();
  double y3 = args[3].getNum();
  state->curveTo(x1, y1, x2, y2, x3, y3);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opCurveTo2(Object args[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    error(errSyntaxError, getPos(), "No current point in curveto2");
    return;
  }
  double x1 = args[0].getNum();
  double y1 = args[1].getNum();
  double x2 = args[2].getNum();
  double y2 = args[3].getNum();
  double x3 = x2;
  double y3 = y2;
  state->curveTo(x1, y1, x2, y2, x3, y3);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opRectangle(Object args[], int /*numArgs*/)
{
  double x = args[0].getNum();
  double y = args[1].getNum();
  double w = args[2].getNum();
  double h = args[3].getNum();
  state->moveTo(x, y);
  state->lineTo(x + w, y);
  state->lineTo(x + w, y + h);
  state->lineTo(x, y + h);
  state->closePath();
}

void PdfParser::opClosePath(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    error(errSyntaxError, getPos(), "No current point in closepath");
    return;
  }
  state->closePath();
}

//------------------------------------------------------------------------
// path painting operators
//------------------------------------------------------------------------

void PdfParser::opEndPath(Object /*args*/[], int /*numArgs*/)
{
  doEndPath();
}

void PdfParser::opStroke(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in stroke"));
    return;
  }
  if (state->isPath()) {
    if (state->getStrokeColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getStrokePattern())) {
          doPatternStrokeFallback();
    } else {
      builder->addPath(state, false, true);
    }
  }
  doEndPath();
}

void PdfParser::opCloseStroke(Object * /*args[]*/, int /*numArgs*/) {
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in closepath/stroke"));
    return;
  }
  state->closePath();
  if (state->isPath()) {
    if (state->getStrokeColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getStrokePattern())) {
      doPatternStrokeFallback();
    } else {
      builder->addPath(state, false, true);
    }
  }
  doEndPath();
}

void PdfParser::opFill(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in fill"));
    return;
  }
  if (state->isPath()) {
    if (state->getFillColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getFillPattern())) {
      doPatternFillFallback(gFalse);
    } else {
      builder->addPath(state, true, false);
    }
  }
  doEndPath();
}

void PdfParser::opEOFill(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in eofill"));
    return;
  }
  if (state->isPath()) {
    if (state->getFillColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getFillPattern())) {
      doPatternFillFallback(gTrue);
    } else {
      builder->addPath(state, true, false, true);
    }
  }
  doEndPath();
}

void PdfParser::opFillStroke(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in fill/stroke"));
    return;
  }
  if (state->isPath()) {
    doFillAndStroke(gFalse);
  } else {
    builder->addPath(state, true, true);
  }
  doEndPath();
}

void PdfParser::opCloseFillStroke(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in closepath/fill/stroke"));
    return;
  }
  if (state->isPath()) {
    state->closePath();
    doFillAndStroke(gFalse);
  }
  doEndPath();
}

void PdfParser::opEOFillStroke(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in eofill/stroke"));
    return;
  }
  if (state->isPath()) {
    doFillAndStroke(gTrue);
  }
  doEndPath();
}

void PdfParser::opCloseEOFillStroke(Object /*args*/[], int /*numArgs*/)
{
  if (!state->isCurPt()) {
    //error(getPos(), const_cast<char*>("No path in closepath/eofill/stroke"));
    return;
  }
  if (state->isPath()) {
    state->closePath();
    doFillAndStroke(gTrue);
  }
  doEndPath();
}

void PdfParser::doFillAndStroke(GBool eoFill) {
    GBool fillOk = gTrue, strokeOk = gTrue;
    if (state->getFillColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getFillPattern())) {
        fillOk = gFalse;
    }
    if (state->getStrokeColorSpace()->getMode() == csPattern &&
        !builder->isPatternTypeSupported(state->getStrokePattern())) {
        strokeOk = gFalse;
    }
    if (fillOk && strokeOk) {
        builder->addPath(state, true, true, eoFill);
    } else {
        doPatternFillFallback(eoFill);
        doPatternStrokeFallback();
    }
}

void PdfParser::doPatternFillFallback(GBool eoFill) {
  GfxPattern *pattern;

  if (!(pattern = state->getFillPattern())) {
    return;
  }
  switch (pattern->getType()) {
  case 1:
    break;
  case 2:
    doShadingPatternFillFallback(static_cast<GfxShadingPattern *>(pattern), gFalse, eoFill);
    break;
  default:
    error(errUnimplemented, getPos(), "Unimplemented pattern type (%d) in fill",
	  pattern->getType());
    break;
  }
}

void PdfParser::doPatternStrokeFallback() {
  GfxPattern *pattern;

  if (!(pattern = state->getStrokePattern())) {
    return;
  }
  switch (pattern->getType()) {
  case 1:
    break;
  case 2:
    doShadingPatternFillFallback(static_cast<GfxShadingPattern *>(pattern), gTrue, gFalse);
    break;
  default:
    error(errUnimplemented, getPos(), "Unimplemented pattern type ({0:d}) in stroke",
	  pattern->getType());
    break;
  }
}

void PdfParser::doShadingPatternFillFallback(GfxShadingPattern *sPat,
                                             GBool stroke, GBool eoFill) {
  GfxShading *shading;
  GfxPath *savedPath;

  shading = sPat->getShading();

  // save current graphics state
  savedPath = state->getPath()->copy();
  saveState();

  // clip to bbox
  /*if (false ){//shading->getHasBBox()) {
    double xMin, yMin, xMax, yMax;
    shading->getBBox(&xMin, &yMin, &xMax, &yMax);
    state->moveTo(xMin, yMin);
    state->lineTo(xMax, yMin);
    state->lineTo(xMax, yMax);
    state->lineTo(xMin, yMax);
    state->closePath();
    state->clip();
    state->setPath(savedPath->copy());
  }*/

  // clip to current path
  if (stroke) {
    state->clipToStrokePath();
  } else {
    state->clip();
    // XXX WARNING WE HAVE REMOVED THE SET CLIP
    /*if (eoFill) {
      builder->setClipPath(state, true);
    } else {
      builder->setClipPath(state);
    }*/
  }

  // set the color space
  state->setFillColorSpace(shading->getColorSpace()->copy());

  // background color fill
  if (shading->getHasBackground()) {
    state->setFillColor(shading->getBackground());
    builder->addPath(state, true, false);
  }
  state->clearPath();

  // construct a (pattern space) -> (current space) transform matrix
  auto ptr = ctmToAffine(sPat->getMatrix());
  auto m = (ptr * baseMatrix) * stateToAffine(state).inverse();

  // Set the new matrix
  state->concatCTM(m[0], m[1], m[2], m[3], m[4], m[5]);

  // do shading type-specific operations
  switch (shading->getType()) {
  case 1:
    doFunctionShFill(static_cast<GfxFunctionShading *>(shading));
    break;
  case 2:
  case 3:
    // no need to implement these
    break;
  case 4:
  case 5:
    doGouraudTriangleShFill(static_cast<GfxGouraudTriangleShading *>(shading));
    break;
  case 6:
  case 7:
    doPatchMeshShFill(static_cast<GfxPatchMeshShading *>(shading));
    break;
  }

  // restore graphics state
  restoreState();
#if POPPLER_CHECK_VERSION(26, 2, 0)
  state->clearPath();
  GfxPath *currPath = const_cast<GfxPath*>(state->getPath());
  currPath->append(savedPath);
#else
  state->setPath(savedPath);
#endif
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opShFill(Object args[], int /*numArgs*/)
{
  GfxPath *savedPath = nullptr;
  bool savedState = false;

  auto shading = std::unique_ptr<GfxShading>(res->lookupShading(args[0].getName(), nullptr, state));
  if (!shading) {
    return;
  }

  // save current graphics state
  if (shading->getType() != 2 && shading->getType() != 3) {
    savedPath = state->getPath()->copy();
    saveState();
    savedState = true;
  }

  // clip to bbox
  /*if (shading->getHasBBox()) {
    double xMin, yMin, xMax, yMax;
    shading->getBBox(&xMin, &yMin, &xMax, &yMax);
    state->moveTo(xMin, yMin);
    state->lineTo(xMax, yMin);
    state->lineTo(xMax, yMax);
    state->lineTo(xMin, yMax);
    state->closePath();
    state->clip();
    builder->setClip(state);
    state->clearPath();
  }*/

  // set the color space
  if (savedState)
    state->setFillColorSpace(shading->getColorSpace()->copy());
  
  

  // do shading type-specific operations
  switch (shading->getType()) {
  case 1: // Function-based shading
    doFunctionShFill(static_cast<GfxFunctionShading *>(shading.get()));
    break;
  case 2: // Axial shading
  case 3: // Radial shading
      builder->addShadedFill(state, shading.get(), stateToAffine(state));
      break;
  case 4: // Free-form Gouraud-shaded triangle mesh
  case 5: // Lattice-form Gouraud-shaded triangle mesh
    doGouraudTriangleShFill(static_cast<GfxGouraudTriangleShading *>(shading.get()));
    break;
  case 6: // Coons patch mesh
  case 7: // Tensor-product patch mesh
    doPatchMeshShFill(static_cast<GfxPatchMeshShading *>(shading.get()));
    break;
  }

  // restore graphics state
  if (savedState) {
    restoreState();
#if POPPLER_CHECK_VERSION(26, 2, 0)
    state->clearPath();
    GfxPath *currPath = const_cast<GfxPath*>(state->getPath());
    currPath->append(savedPath);
#else
    state->setPath(savedPath);
#endif
  }
}

void PdfParser::doFunctionShFill(GfxFunctionShading *shading) {
  double x0, y0, x1, y1;
  GfxColor colors[4];

  shading->getDomain(&x0, &y0, &x1, &y1);
  shading->getColor(x0, y0, &colors[0]);
  shading->getColor(x0, y1, &colors[1]);
  shading->getColor(x1, y0, &colors[2]);
  shading->getColor(x1, y1, &colors[3]);
  doFunctionShFill1(shading, x0, y0, x1, y1, colors, 0);
}

void PdfParser::doFunctionShFill1(GfxFunctionShading *shading,
			    double x0, double y0,
			    double x1, double y1,
			    GfxColor *colors, int depth) {
  GfxColor fillColor;
  GfxColor color0M, color1M, colorM0, colorM1, colorMM;
  GfxColor colors2[4];
  double xM, yM;
  int nComps, i, j;

  nComps = shading->getColorSpace()->getNComps();
  const auto& matrix = shading->getMatrix();

  // compare the four corner colors
  for (i = 0; i < 4; ++i) {
    for (j = 0; j < nComps; ++j) {
      if (abs(colors[i].c[j] - colors[(i+1)&3].c[j]) > colorDelta) {
	break;
      }
    }
    if (j < nComps) {
      break;
    }
  }

  // center of the rectangle
  xM = 0.5 * (x0 + x1);
  yM = 0.5 * (y0 + y1);

  // the four corner colors are close (or we hit the recursive limit)
  // -- fill the rectangle; but require at least one subdivision
  // (depth==0) to avoid problems when the four outer corners of the
  // shaded region are the same color
  if ((i == 4 && depth > 0) || depth == maxDepth) {

    // use the center color
    shading->getColor(xM, yM, &fillColor);
    state->_POPPLER_SET_FILL_COLOR(fillColor);

    // fill the rectangle
    state->moveTo(x0 * matrix[0] + y0 * matrix[2] + matrix[4],
		  x0 * matrix[1] + y0 * matrix[3] + matrix[5]);
    state->lineTo(x1 * matrix[0] + y0 * matrix[2] + matrix[4],
		  x1 * matrix[1] + y0 * matrix[3] + matrix[5]);
    state->lineTo(x1 * matrix[0] + y1 * matrix[2] + matrix[4],
		  x1 * matrix[1] + y1 * matrix[3] + matrix[5]);
    state->lineTo(x0 * matrix[0] + y1 * matrix[2] + matrix[4],
		  x0 * matrix[1] + y1 * matrix[3] + matrix[5]);
    state->closePath();
    builder->addPath(state, true, false);
    state->clearPath();

  // the four corner colors are not close enough -- subdivide the
  // rectangle
  } else {

    // colors[0]       colorM0       colors[2]
    //   (x0,y0)       (xM,y0)       (x1,y0)
    //         +----------+----------+
    //         |          |          |
    //         |    UL    |    UR    |
    // color0M |       colorMM       | color1M
    // (x0,yM) +----------+----------+ (x1,yM)
    //         |       (xM,yM)       |
    //         |    LL    |    LR    |
    //         |          |          |
    //         +----------+----------+
    // colors[1]       colorM1       colors[3]
    //   (x0,y1)       (xM,y1)       (x1,y1)

    shading->getColor(x0, yM, &color0M);
    shading->getColor(x1, yM, &color1M);
    shading->getColor(xM, y0, &colorM0);
    shading->getColor(xM, y1, &colorM1);
    shading->getColor(xM, yM, &colorMM);

    // upper-left sub-rectangle
    colors2[0] = colors[0];
    colors2[1] = color0M;
    colors2[2] = colorM0;
    colors2[3] = colorMM;
    doFunctionShFill1(shading, x0, y0, xM, yM, colors2, depth + 1);
    
    // lower-left sub-rectangle
    colors2[0] = color0M;
    colors2[1] = colors[1];
    colors2[2] = colorMM;
    colors2[3] = colorM1;
    doFunctionShFill1(shading, x0, yM, xM, y1, colors2, depth + 1);
    
    // upper-right sub-rectangle
    colors2[0] = colorM0;
    colors2[1] = colorMM;
    colors2[2] = colors[2];
    colors2[3] = color1M;
    doFunctionShFill1(shading, xM, y0, x1, yM, colors2, depth + 1);

    // lower-right sub-rectangle
    colors2[0] = colorMM;
    colors2[1] = colorM1;
    colors2[2] = color1M;
    colors2[3] = colors[3];
    doFunctionShFill1(shading, xM, yM, x1, y1, colors2, depth + 1);
  }
}

void PdfParser::doGouraudTriangleShFill(GfxGouraudTriangleShading *shading) {
  // adapted from poppler/Gfx.cc
  double x0, y0, x1, y1, x2, y2;
  if (shading->isParameterized()) {
    double color0, color1, color2;
    // a relative threshold, also copied from poppler/Gfx.cc
    const double refineColorThreshold = gouraudParameterizedColorDelta * 
                                        (shading->getParameterDomainMax() - shading->getParameterDomainMin());
    for (int i = 0; i < shading->getNTriangles(); ++i) {
        shading->getTriangle(i, &x0, &y0, &color0, &x1, &y1, &color1, &x2, &y2, &color2);
        gouraudFillTriangle(x0, y0, color0, x1, y1, color1, x2, y2, color2, refineColorThreshold, 0, shading);
    }
  } else {
    GfxColor color0, color1, color2;

    for (int i = 0; i < shading->getNTriangles(); ++i) {
      shading->getTriangle(i, &x0, &y0, &color0, &x1, &y1, &color1, &x2, &y2, &color2);
      gouraudFillTriangle(x0, y0, &color0, x1, y1, &color1, x2, y2, &color2,
                          shading->getColorSpace()->getNComps(), 0);
    }
  }
}

void PdfParser::gouraudFillTriangle(double x0, double y0, double color0, 
                                    double x1, double y1, double color1, 
                                    double x2, double y2, double color2, 
                                    double refineColorThreshold, int depth, 
                                    GfxGouraudTriangleShading *shading)
{
    const double meanColor = (color0 + color1 + color2) / 3;

    const bool isFineEnough = fabs(color0 - meanColor) < refineColorThreshold && 
      fabs(color1 - meanColor) < refineColorThreshold && fabs(color2 - meanColor) < refineColorThreshold;

    if (isFineEnough || depth == maxDepth) {
        GfxColor color;
        shading->getParameterizedColor(meanColor, &color);
        state->_POPPLER_SET_FILL_COLOR(color);
        state->moveTo(x0, y0);
        state->lineTo(x1, y1);
        state->lineTo(x2, y2);
        state->closePath();
        builder->addPath(state, true, false);
        state->clearPath();
    } else {
        const double x01 = 0.5 * (x0 + x1);
        const double y01 = 0.5 * (y0 + y1);
        const double x12 = 0.5 * (x1 + x2);
        const double y12 = 0.5 * (y1 + y2);
        const double x20 = 0.5 * (x2 + x0);
        const double y20 = 0.5 * (y2 + y0);
        const double color01 = (color0 + color1) / 2.;
        const double color12 = (color1 + color2) / 2.;
        const double color20 = (color2 + color0) / 2.;
        ++depth;
        gouraudFillTriangle(x0, y0, color0, x01, y01, color01, x20, y20, color20, refineColorThreshold, depth, shading);
        gouraudFillTriangle(x01, y01, color01, x1, y1, color1, x12, y12, color12, refineColorThreshold, depth, shading);
        gouraudFillTriangle(x01, y01, color01, x12, y12, color12, x20, y20, color20, refineColorThreshold, depth, shading);
        gouraudFillTriangle(x20, y20, color20, x12, y12, color12, x2, y2, color2, refineColorThreshold, depth, shading);
    }
}

void PdfParser::gouraudFillTriangle(double x0, double y0, GfxColor *color0,
			      double x1, double y1, GfxColor *color1,
			      double x2, double y2, GfxColor *color2,
			      int nComps, int depth) {
  double x01, y01, x12, y12, x20, y20;
  GfxColor color01, color12, color20;
  int i;

  for (i = 0; i < nComps; ++i) {
    if (abs(color0->c[i] - color1->c[i]) > colorDelta ||
       abs(color1->c[i] - color2->c[i]) > colorDelta) {
      break;
    }
  }
  if (i == nComps || depth == maxDepth) {
    state->_POPPLER_SET_FILL_COLOR(*color0);
    state->moveTo(x0, y0);
    state->lineTo(x1, y1);
    state->lineTo(x2, y2);
    state->closePath();
    builder->addPath(state, true, false);
    state->clearPath();
  } else {
    x01 = 0.5 * (x0 + x1);
    y01 = 0.5 * (y0 + y1);
    x12 = 0.5 * (x1 + x2);
    y12 = 0.5 * (y1 + y2);
    x20 = 0.5 * (x2 + x0);
    y20 = 0.5 * (y2 + y0);
    //~ if the shading has a Function, this should interpolate on the
    //~ function parameter, not on the color components
    for (i = 0; i < nComps; ++i) {
      color01.c[i] = (color0->c[i] + color1->c[i]) / 2;
      color12.c[i] = (color1->c[i] + color2->c[i]) / 2;
      color20.c[i] = (color2->c[i] + color0->c[i]) / 2;
    }
    gouraudFillTriangle(x0, y0, color0, x01, y01, &color01,
			x20, y20, &color20, nComps, depth + 1);
    gouraudFillTriangle(x01, y01, &color01, x1, y1, color1,
			x12, y12, &color12, nComps, depth + 1);
    gouraudFillTriangle(x01, y01, &color01, x12, y12, &color12,
			x20, y20, &color20, nComps, depth + 1);
    gouraudFillTriangle(x20, y20, &color20, x12, y12, &color12,
			x2, y2, color2, nComps, depth + 1);
  }
}

void PdfParser::doPatchMeshShFill(GfxPatchMeshShading *shading) {
  int start, i;

  if (shading->getNPatches() > 128) {
    start = 3;
  } else if (shading->getNPatches() > 64) {
    start = 2;
  } else if (shading->getNPatches() > 16) {
    start = 1;
  } else {
    start = 0;
  }
  for (i = 0; i < shading->getNPatches(); ++i) {
    fillPatch(shading->getPatch(i), shading->getColorSpace()->getNComps(),
	      start);
  }
}

void PdfParser::fillPatch(_POPPLER_CONST GfxPatch *patch, int nComps, int depth) {
  GfxPatch patch00 = blankPatch();
  GfxPatch patch01 = blankPatch();
  GfxPatch patch10 = blankPatch();
  GfxPatch patch11 = blankPatch();
  GfxColor color = {{0}};
  double xx[4][8];
  double yy[4][8];
  double xxm;
  double yym;

  int i;

  for (i = 0; i < nComps; ++i) {
    if (std::abs(patch->color[0][0].c[i] - patch->color[0][1].c[i])
	  > colorDelta ||
	std::abs(patch->color[0][1].c[i] - patch->color[1][1].c[i])
	  > colorDelta ||
	std::abs(patch->color[1][1].c[i] - patch->color[1][0].c[i])
	  > colorDelta ||
	std::abs(patch->color[1][0].c[i] - patch->color[0][0].c[i])
	  > colorDelta) {
      break;
    }
    color.c[i] = GfxColorComp(patch->color[0][0].c[i]);
  }
  if (i == nComps || depth == maxDepth) {
    state->_POPPLER_SET_FILL_COLOR(color);
    state->moveTo(patch->x[0][0], patch->y[0][0]);
    state->curveTo(patch->x[0][1], patch->y[0][1],
		   patch->x[0][2], patch->y[0][2],
		   patch->x[0][3], patch->y[0][3]);
    state->curveTo(patch->x[1][3], patch->y[1][3],
		   patch->x[2][3], patch->y[2][3],
		   patch->x[3][3], patch->y[3][3]);
    state->curveTo(patch->x[3][2], patch->y[3][2],
		   patch->x[3][1], patch->y[3][1],
		   patch->x[3][0], patch->y[3][0]);
    state->curveTo(patch->x[2][0], patch->y[2][0],
		   patch->x[1][0], patch->y[1][0],
		   patch->x[0][0], patch->y[0][0]);
    state->closePath();
    builder->addPath(state, true, false);
    state->clearPath();
  } else {
    for (i = 0; i < 4; ++i) {
      xx[i][0] = patch->x[i][0];
      yy[i][0] = patch->y[i][0];
      xx[i][1] = 0.5 * (patch->x[i][0] + patch->x[i][1]);
      yy[i][1] = 0.5 * (patch->y[i][0] + patch->y[i][1]);
      xxm = 0.5 * (patch->x[i][1] + patch->x[i][2]);
      yym = 0.5 * (patch->y[i][1] + patch->y[i][2]);
      xx[i][6] = 0.5 * (patch->x[i][2] + patch->x[i][3]);
      yy[i][6] = 0.5 * (patch->y[i][2] + patch->y[i][3]);
      xx[i][2] = 0.5 * (xx[i][1] + xxm);
      yy[i][2] = 0.5 * (yy[i][1] + yym);
      xx[i][5] = 0.5 * (xxm + xx[i][6]);
      yy[i][5] = 0.5 * (yym + yy[i][6]);
      xx[i][3] = xx[i][4] = 0.5 * (xx[i][2] + xx[i][5]);
      yy[i][3] = yy[i][4] = 0.5 * (yy[i][2] + yy[i][5]);
      xx[i][7] = patch->x[i][3];
      yy[i][7] = patch->y[i][3];
    }
    for (i = 0; i < 4; ++i) {
      patch00.x[0][i] = xx[0][i];
      patch00.y[0][i] = yy[0][i];
      patch00.x[1][i] = 0.5 * (xx[0][i] + xx[1][i]);
      patch00.y[1][i] = 0.5 * (yy[0][i] + yy[1][i]);
      xxm = 0.5 * (xx[1][i] + xx[2][i]);
      yym = 0.5 * (yy[1][i] + yy[2][i]);
      patch10.x[2][i] = 0.5 * (xx[2][i] + xx[3][i]);
      patch10.y[2][i] = 0.5 * (yy[2][i] + yy[3][i]);
      patch00.x[2][i] = 0.5 * (patch00.x[1][i] + xxm);
      patch00.y[2][i] = 0.5 * (patch00.y[1][i] + yym);
      patch10.x[1][i] = 0.5 * (xxm + patch10.x[2][i]);
      patch10.y[1][i] = 0.5 * (yym + patch10.y[2][i]);
      patch00.x[3][i] = 0.5 * (patch00.x[2][i] + patch10.x[1][i]);
      patch00.y[3][i] = 0.5 * (patch00.y[2][i] + patch10.y[1][i]);
      patch10.x[0][i] = patch00.x[3][i];
      patch10.y[0][i] = patch00.y[3][i];
      patch10.x[3][i] = xx[3][i];
      patch10.y[3][i] = yy[3][i];
    }
    for (i = 4; i < 8; ++i) {
      patch01.x[0][i-4] = xx[0][i];
      patch01.y[0][i-4] = yy[0][i];
      patch01.x[1][i-4] = 0.5 * (xx[0][i] + xx[1][i]);
      patch01.y[1][i-4] = 0.5 * (yy[0][i] + yy[1][i]);
      xxm = 0.5 * (xx[1][i] + xx[2][i]);
      yym = 0.5 * (yy[1][i] + yy[2][i]);
      patch11.x[2][i-4] = 0.5 * (xx[2][i] + xx[3][i]);
      patch11.y[2][i-4] = 0.5 * (yy[2][i] + yy[3][i]);
      patch01.x[2][i-4] = 0.5 * (patch01.x[1][i-4] + xxm);
      patch01.y[2][i-4] = 0.5 * (patch01.y[1][i-4] + yym);
      patch11.x[1][i-4] = 0.5 * (xxm + patch11.x[2][i-4]);
      patch11.y[1][i-4] = 0.5 * (yym + patch11.y[2][i-4]);
      patch01.x[3][i-4] = 0.5 * (patch01.x[2][i-4] + patch11.x[1][i-4]);
      patch01.y[3][i-4] = 0.5 * (patch01.y[2][i-4] + patch11.y[1][i-4]);
      patch11.x[0][i-4] = patch01.x[3][i-4];
      patch11.y[0][i-4] = patch01.y[3][i-4];
      patch11.x[3][i-4] = xx[3][i];
      patch11.y[3][i-4] = yy[3][i];
    }
    //~ if the shading has a Function, this should interpolate on the
    //~ function parameter, not on the color components
    for (i = 0; i < nComps; ++i) {
      patch00.color[0][0].c[i] = patch->color[0][0].c[i];
      patch00.color[0][1].c[i] = (patch->color[0][0].c[i] +
				  patch->color[0][1].c[i]) / 2;
      patch01.color[0][0].c[i] = patch00.color[0][1].c[i];
      patch01.color[0][1].c[i] = patch->color[0][1].c[i];
      patch01.color[1][1].c[i] = (patch->color[0][1].c[i] +
				  patch->color[1][1].c[i]) / 2;
      patch11.color[0][1].c[i] = patch01.color[1][1].c[i];
      patch11.color[1][1].c[i] = patch->color[1][1].c[i];
      patch11.color[1][0].c[i] = (patch->color[1][1].c[i] +
				  patch->color[1][0].c[i]) / 2;
      patch10.color[1][1].c[i] = patch11.color[1][0].c[i];
      patch10.color[1][0].c[i] = patch->color[1][0].c[i];
      patch10.color[0][0].c[i] = (patch->color[1][0].c[i] +
				  patch->color[0][0].c[i]) / 2;
      patch00.color[1][0].c[i] = patch10.color[0][0].c[i];
      patch00.color[1][1].c[i] = (patch00.color[1][0].c[i] +
				  patch01.color[1][1].c[i]) / 2;
      patch01.color[1][0].c[i] = patch00.color[1][1].c[i];
      patch11.color[0][0].c[i] = patch00.color[1][1].c[i];
      patch10.color[0][1].c[i] = patch00.color[1][1].c[i];
    }
    fillPatch(&patch00, nComps, depth + 1);
    fillPatch(&patch10, nComps, depth + 1);
    fillPatch(&patch01, nComps, depth + 1);
    fillPatch(&patch11, nComps, depth + 1);
  }
}

void PdfParser::doEndPath() {
    if (state->isCurPt() && clip != clipNone) {
        state->clip();
        builder->setClip(state, clip);
        clip = clipNone;
    }
    state->clearPath();
}

//------------------------------------------------------------------------
// path clipping operators
//------------------------------------------------------------------------

void PdfParser::opClip(Object /*args*/[], int /*numArgs*/)
{
  clip = clipNormal;
}

void PdfParser::opEOClip(Object /*args*/[], int /*numArgs*/)
{
  clip = clipEO;
}

//------------------------------------------------------------------------
// text object operators
//------------------------------------------------------------------------

void PdfParser::opBeginText(Object /*args*/[], int /*numArgs*/)
{
  state->setTextMat(1, 0, 0, 1, 0, 0);
  state->textMoveTo(0, 0);
  builder->updateTextPosition(0.0, 0.0);
  fontChanged = gTrue;
  builder->beginTextObject(state);
}

void PdfParser::opEndText(Object /*args*/[], int /*numArgs*/)
{
  builder->endTextObject(state);
}

//------------------------------------------------------------------------
// text state operators
//------------------------------------------------------------------------

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetCharSpacing(Object args[], int /*numArgs*/)
{
  state->setCharSpace(args[0].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetFont(Object args[], int /*numArgs*/)
{
  auto font = res->lookupFont(args[0].getName());

  if (!font) {
    // unsetting the font (drawing no text) is better than using the
    // previous one and drawing random glyphs from it
    state->setFont(nullptr, args[1].getNum());
    fontChanged = gTrue;
    return;
  }
  if (printCommands) {
    printf("  font: tag=%s name='%s' %g\n",
#if POPPLER_CHECK_VERSION(21,11,0)
	   font->getTag().c_str(),
#else
	   font->getTag()->getCString(),
#endif
	   font->getName() ? font->getName()->getCString() : "???",
	   args[1].getNum());
    fflush(stdout);
  }

#if !POPPLER_CHECK_VERSION(22, 4, 0)
  font->incRefCnt();
#endif
  state->setFont(font, args[1].getNum());
  fontChanged = gTrue;
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetTextLeading(Object args[], int /*numArgs*/)
{
  state->setLeading(args[0].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetTextRender(Object args[], int /*numArgs*/)
{
  builder->beforeStateChange(state);
  state->setRender(args[0].getInt());
  builder->updateStyle(state);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetTextRise(Object args[], int /*numArgs*/)
{
  state->setRise(args[0].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetWordSpacing(Object args[], int /*numArgs*/)
{
  state->setWordSpace(args[0].getNum());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetHorizScaling(Object args[], int /*numArgs*/)
{
  state->setHorizScaling(args[0].getNum());
  builder->updateTextMatrix(state, !subPage);
  fontChanged = gTrue;
}

//------------------------------------------------------------------------
// text positioning operators
//------------------------------------------------------------------------

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opTextMove(Object args[], int /*numArgs*/)
{
  double tx, ty;

  tx = state->getLineX() + args[0].getNum();
  ty = state->getLineY() + args[1].getNum();
  state->textMoveTo(tx, ty);
  builder->updateTextPosition(tx, ty);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opTextMoveSet(Object args[], int /*numArgs*/)
{
  double tx, ty;

  tx = state->getLineX() + args[0].getNum();
  ty = args[1].getNum();
  state->setLeading(-ty);
  ty += state->getLineY();
  state->textMoveTo(tx, ty);
  builder->updateTextPosition(tx, ty);
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opSetTextMatrix(Object args[], int /*numArgs*/)
{
  state->setTextMat(args[0].getNum(), args[1].getNum(),
		    args[2].getNum(), args[3].getNum(),
		    args[4].getNum(), args[5].getNum());
  state->textMoveTo(0, 0);
  builder->updateTextMatrix(state, !subPage);
  builder->updateTextPosition(0.0, 0.0);
  fontChanged = gTrue;
}

void PdfParser::opTextNextLine(Object /*args*/[], int /*numArgs*/)
{
  double tx, ty;

  tx = state->getLineX();
  ty = state->getLineY() - state->getLeading();
  state->textMoveTo(tx, ty);
  builder->updateTextPosition(tx, ty);
}

//------------------------------------------------------------------------
// text string operators
//------------------------------------------------------------------------

void PdfParser::doUpdateFont()
{
    if (fontChanged) {
        auto font = getFontEngine()->getFont(state->getFont(), _pdf_doc.get(), true, xref);
        builder->updateFont(state, font, !subPage);
        fontChanged = false;
    }
}

std::shared_ptr<CairoFontEngine> PdfParser::getFontEngine()
{
    // poppler/CairoOutputDev.cc claims the FT Library needs to be kept around
    // for a while. It's unclear if this is sure for our case.
    static FT_Library ft_lib;
    static std::once_flag ft_lib_once_flag;
    std::call_once(ft_lib_once_flag, FT_Init_FreeType, &ft_lib);
    if (!_font_engine) {
        // This will make a new font engine per form1, in the future we could
        // share this between PdfParser instances for the same PDF file.
        _font_engine = std::make_shared<CairoFontEngine>(ft_lib);
    }
    return _font_engine;
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opShowText(Object args[], int /*numArgs*/)
{
  if (!state->getFont()) {
    error(errSyntaxError, getPos(), "No font in show");
    return;
  }
  doUpdateFont();
  doShowText(args[0].getString());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opMoveShowText(Object args[], int /*numArgs*/)
{
  double tx = 0;
  double ty = 0;

  if (!state->getFont()) {
    error(errSyntaxError, getPos(), "No font in move/show");
    return;
  }
  doUpdateFont();
  tx = state->getLineX();
  ty = state->getLineY() - state->getLeading();
  state->textMoveTo(tx, ty);
  builder->updateTextPosition(tx, ty);
  doShowText(args[0].getString());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opMoveSetShowText(Object args[], int /*numArgs*/)
{
  double tx = 0;
  double ty = 0;

  if (!state->getFont()) {
    error(errSyntaxError, getPos(), "No font in move/set/show");
    return;
  }
  doUpdateFont();
  state->setWordSpace(args[0].getNum());
  state->setCharSpace(args[1].getNum());
  tx = state->getLineX();
  ty = state->getLineY() - state->getLeading();
  state->textMoveTo(tx, ty);
  builder->updateTextPosition(tx, ty);
  doShowText(args[2].getString());
}

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opShowSpaceText(Object args[], int /*numArgs*/)
{
  Array *a = nullptr;
  Object obj;
  _POPPLER_WMODE wMode = _POPPLER_WMODE_HORIZONTAL; // Writing mode (horizontal/vertical).

  if (!state->getFont()) {
    error(errSyntaxError, getPos(), "No font in show/space");
    return;
  }
  doUpdateFont();
  wMode = state->getFont()->getWMode();
  a = args[0].getArray();
  for (int i = 0; i < a->getLength(); ++i) {
    _POPPLER_CALL_ARGS(obj, a->get, i);
    if (obj.isNum()) {
      // this uses the absolute value of the font size to match
      // Acrobat's behavior
      if (wMode != _POPPLER_WMODE_HORIZONTAL) {
	state->textShift(0, -obj.getNum() * 0.001 *
			    fabs(state->getFontSize()));
      } else {
	state->textShift(-obj.getNum() * 0.001 *
			 fabs(state->getFontSize()), 0);
      }
      builder->updateTextShift(state, obj.getNum());
    } else if (obj.isString()) {
      doShowText(obj.getString());
    } else {
      error(errSyntaxError, getPos(), "Element of show/space array must be number or string");
    }
    _POPPLER_FREE(obj);
  }
}

/*
 * This adds a string from a PDF file that is contained in one command ('Tj', ''', '"')
 * or is one string in ShowSpacetext ('TJ').
 */
void PdfParser::doShowText(const std::string &s) {
    auto font = state->getFont();
    _POPPLER_WMODE wMode = font->getWMode(); // Vertical/Horizontal/Invalid

    builder->beginString(state, s.size());

    // handle a Type 3 char
    if (font->getType() == fontType3) {
        g_warning("PDF fontType3 information ignored.");
    }

    double riseX, riseY;
    state->textTransformDelta(0, state->getRise(), &riseX, &riseY);

    auto p = s.c_str(); // char* or const char*
    int len = s.size();

    while (len > 0) {

        CharCode code;                          // Font code (8-bit char code, 16 bit CID, etc.).
        Unicode _POPPLER_CONST_82 *u = nullptr; // Unicode mapping of 'code' (if toUnicode table exists).
        int uLen;
        double dx, dy;           // Displacement vector (e.g. advance).
        double originX, originY; // Origin offset.

        // Get next unicode character, returning number of bytes used.
        int n = font->getNextChar(p, len, &code, &u, &uLen, &dx, &dy, &originX, &originY);

        dx *= state->getFontSize();
        dy *= state->getFontSize();
        originX *= state->getFontSize();
        originY *= state->getFontSize();

        // Save advances for SVG output with 'dx' and 'dy' attributes.
        auto ax = dx;
        auto ay = dy;

        if (wMode != _POPPLER_WMODE_HORIZONTAL) {
            // Vertical text (or invalid value).
            dy += state->getCharSpace();
            if (n == 1 && *p == ' ') {
                dy += state->getWordSpace();
            }
        } else {
            // Horizontal text.
            dx += state->getCharSpace();
            if (n == 1 && *p == ' ') {
                dx += state->getWordSpace();
            }
            dx *= state->getHorizScaling(); // Applies to glyphs and char/word spacing.
            ax *= state->getHorizScaling();
        }

        double tdx, tdy;
        state->textTransformDelta(dx, dy, &tdx, &tdy);

        double tOriginX, tOriginY;
        state->textTransformDelta(originX, originY, &tOriginX, &tOriginY);

        // In Gfx.cc this is drawChar(...)
        builder->addChar(state, state->_POPPLER_GET_CUR_TEXT_X() + riseX, state->_POPPLER_GET_CUR_TEXT_Y() + riseY,
                         dx, dy, ax, ay, tOriginX, tOriginY, code, n, u, uLen);

        // Move onto next unicode character.
        state->_POPPLER_TEXT_SHIFT_WITH_USER_COORDS(tdx, tdy);
        p += n;
        len -= n;
    }

    builder->endString(state);
}

#if POPPLER_CHECK_VERSION(0,64,0)
void PdfParser::doShowText(const GooString *s) {
#else
void PdfParser::doShowText(GooString *s) {
#endif
    const std::string str = s->toStr();
    doShowText(str);
}


//------------------------------------------------------------------------
// XObject operators
//------------------------------------------------------------------------

// TODO not good that numArgs is ignored but args[] is used:
void PdfParser::opXObject(Object args[], int /*numArgs*/)
{
    Object obj1, obj2, obj3, refObj;
    bool layered = false;

#if POPPLER_CHECK_VERSION(0,64,0)
    const char *name = args[0].getName();
#else
    char *name = args[0].getName();
#endif
    _POPPLER_CALL_ARGS(obj1, res->lookupXObject, name);
    if (obj1.isNull()) {
        return;
    }
    if (!obj1.isStream()) {
        error(errSyntaxError, getPos(), "XObject '{0:s}' is wrong type", name);
        _POPPLER_FREE(obj1);
        return;
    }

//add layer at root if xObject has type OCG
    _POPPLER_CALL_ARGS(obj2, obj1.getStream()->getDict()->lookup, "OC");
    if(obj2.isDict()){
        auto type_dict = obj2.getDict();
        if (type_dict->lookup("Type").isName("OCG")) {
            std::string label = getDictString(type_dict, "Name");
            builder->beginXObjectLayer(label);
            layered = true;
        }
    }

    _POPPLER_CALL_ARGS(obj2, obj1.getStream()->getDict()->lookup, "Subtype");
    if (obj2.isName(const_cast<char*>("Image"))) {
        _POPPLER_CALL_ARGS(refObj, res->lookupXObjectNF, name);
        doImage(&refObj, obj1.getStream(), gFalse);
        _POPPLER_FREE(refObj);
    } else if (obj2.isName(const_cast<char*>("Form"))) {
        doForm(&obj1);
    } else if (obj2.isName(const_cast<char*>("PS"))) {
        _POPPLER_CALL_ARGS(obj3, obj1.getStream()->getDict()->lookup, "Level1");
    } else if (obj2.isName()) {
        error(errSyntaxError, getPos(), "Unknown XObject subtype '{0:s}'", obj2.getName());
    } else {
        error(errSyntaxError, getPos(), "XObject subtype is missing or wrong type");
    }

    //End XObject layer if OC of type OCG is present
    if (layered) {
        builder->endMarkedContent();
    }

    _POPPLER_FREE(obj2);
    _POPPLER_FREE(obj1);
}

void PdfParser::doImage(Object * /*ref*/, Stream *str, GBool inlineImg)
{
    Dict *dict;
    int width, height;
    int bits;
    GBool interpolate;
    StreamColorSpaceMode csMode;
    GBool hasAlpha;
    GBool mask;
    GBool invert;
    Object maskObj, smaskObj;
    GBool haveColorKeyMask, haveExplicitMask, haveSoftMask;
    GBool maskInvert;
    GBool maskInterpolate;
    Object obj1, obj2;
    
    // get info from the stream
    bits = 0;
    csMode = streamCSNone;
    hasAlpha = false;
    str->_POPPLER_GET_IMAGE_PARAMS(&bits, &csMode, &hasAlpha);
    
    // get stream dict
    dict = str->getDict();
    
    // get size
    _POPPLER_CALL_ARGS(obj1, dict->lookup, "Width");
    if (obj1.isNull()) {
        _POPPLER_FREE(obj1);
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "W");
    }
    if (obj1.isInt()){
        width = obj1.getInt();
    }
    else if (obj1.isReal()) {
        width = (int)obj1.getReal();
    }
    else {
        goto err2;
    }
    _POPPLER_FREE(obj1);
    _POPPLER_CALL_ARGS(obj1, dict->lookup, "Height");
    if (obj1.isNull()) {
        _POPPLER_FREE(obj1);
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "H");
    }
    if (obj1.isInt()) {
        height = obj1.getInt();
    }
    else if (obj1.isReal()){
        height = static_cast<int>(obj1.getReal());
    }
    else {
        goto err2;
    }
    _POPPLER_FREE(obj1);
    
    // image interpolation
    _POPPLER_CALL_ARGS(obj1, dict->lookup, "Interpolate");
    if (obj1.isNull()) {
      _POPPLER_FREE(obj1);
      _POPPLER_CALL_ARGS(obj1, dict->lookup, "I");
    }
    if (obj1.isBool())
      interpolate = obj1.getBool();
    else
      interpolate = gFalse;
    _POPPLER_FREE(obj1);
    maskInterpolate = gFalse;

    // image or mask?
    _POPPLER_CALL_ARGS(obj1, dict->lookup, "ImageMask");
    if (obj1.isNull()) {
        _POPPLER_FREE(obj1);
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "IM");
    }
    mask = gFalse;
    if (obj1.isBool()) {
        mask = obj1.getBool();
    }
    else if (!obj1.isNull()) {
        goto err2;
    }
    _POPPLER_FREE(obj1);
    
    // bit depth
    if (bits == 0) {
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "BitsPerComponent");
        if (obj1.isNull()) {
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, dict->lookup, "BPC");
        }
        if (obj1.isInt()) {
            bits = obj1.getInt();
        } else if (mask) {
            bits = 1;
        } else {
            goto err2;
        }
        _POPPLER_FREE(obj1);
    }
    
    // display a mask
    if (mask) {
        // check for inverted mask
        if (bits != 1) {
            goto err1;
        }
        invert = gFalse;
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "Decode");
        if (obj1.isNull()) {
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, dict->lookup, "D");
        }
        if (obj1.isArray()) {
            _POPPLER_CALL_ARGS(obj2, obj1.getArray()->get, 0);
            if (obj2.isInt() && obj2.getInt() == 1) {
                invert = gTrue;
            }
            _POPPLER_FREE(obj2);
        } else if (!obj1.isNull()) {
            goto err2;
        }
        _POPPLER_FREE(obj1);
        
        // draw it
        builder->addImageMask(state, str, width, height, invert, interpolate);
        
    } else {
        // get color space and color map
        std::unique_ptr<GfxColorSpace> colorSpace;
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "ColorSpace");
        if (obj1.isNull()) {
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, dict->lookup, "CS");
        }
        if (!obj1.isNull()) {
            colorSpace = lookupColorSpaceCopy(obj1);
        } else if (csMode == streamCSDeviceGray) {
            colorSpace = std::make_unique<GfxDeviceGrayColorSpace>();
        } else if (csMode == streamCSDeviceRGB) {
            colorSpace = std::make_unique<GfxDeviceRGBColorSpace>();
        } else if (csMode == streamCSDeviceCMYK) {
            colorSpace = std::make_unique<GfxDeviceCMYKColorSpace>();
        }
        _POPPLER_FREE(obj1);
        if (!colorSpace) {
            goto err1;
        }
        _POPPLER_CALL_ARGS(obj1, dict->lookup, "Decode");
        if (obj1.isNull()) {
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, dict->lookup, "D");
        }
        auto colorMap = std::make_unique<GfxImageColorMap>(bits, &obj1, _POPPLER_CONSUME_UNIQPTR_ARG(colorSpace));
        _POPPLER_FREE(obj1);
        if (!colorMap->isOk()) {
            goto err1;
        }
        
        // get the mask
        int maskColors[2*gfxColorMaxComps];
        haveColorKeyMask = haveExplicitMask = haveSoftMask = gFalse;
        Stream *maskStr = nullptr;
        int maskWidth = 0;
        int maskHeight = 0;
        maskInvert = gFalse;
        std::unique_ptr<GfxImageColorMap> maskColorMap;
        _POPPLER_CALL_ARGS(maskObj, dict->lookup, "Mask");
        _POPPLER_CALL_ARGS(smaskObj, dict->lookup, "SMask");
        Dict* maskDict;
        if (smaskObj.isStream()) {
            // soft mask
            if (inlineImg) {
	            goto err1;
            }
            maskStr = smaskObj.getStream();
            maskDict = smaskObj.getStream()->getDict();
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Width");
            if (obj1.isNull()) {
                    _POPPLER_FREE(obj1);
	            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "W");
            }
            if (!obj1.isInt()) {
	            goto err2;
            }
            maskWidth = obj1.getInt();
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Height");
            if (obj1.isNull()) {
	            _POPPLER_FREE(obj1);
                    _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "H");
            }
            if (!obj1.isInt()) {
	            goto err2;
            }
            maskHeight = obj1.getInt();
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "BitsPerComponent");
            if (obj1.isNull()) {
                    _POPPLER_FREE(obj1);
                    _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "BPC");
            }
            if (!obj1.isInt()) {
	            goto err2;
            }
            int maskBits = obj1.getInt();
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Interpolate");
	    if (obj1.isNull()) {
	      _POPPLER_FREE(obj1);
              _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "I");
	    }
	    if (obj1.isBool())
	      maskInterpolate = obj1.getBool();
	    else
	      maskInterpolate = gFalse;
	    _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "ColorSpace");
            if (obj1.isNull()) {
	            _POPPLER_FREE(obj1);
                    _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "CS");
            }
            auto maskColorSpace = lookupColorSpaceCopy(obj1);
            _POPPLER_FREE(obj1);
            if (!maskColorSpace || maskColorSpace->getMode() != csDeviceGray) {
                goto err1;
            }
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Decode");
            if (obj1.isNull()) {
                _POPPLER_FREE(obj1);
                _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "D");
            }
            maskColorMap = std::make_unique<GfxImageColorMap>(maskBits, &obj1, _POPPLER_CONSUME_UNIQPTR_ARG(maskColorSpace));
            _POPPLER_FREE(obj1);
            if (!maskColorMap->isOk()) {
                goto err1;
            }
            //~ handle the Matte entry
            haveSoftMask = gTrue;
        } else if (maskObj.isArray()) {
            // color key mask
            int i;
            for (i = 0; i < maskObj.getArray()->getLength() && i < 2*gfxColorMaxComps; ++i) {
                _POPPLER_CALL_ARGS(obj1, maskObj.getArray()->get, i);
                maskColors[i] = obj1.getInt();
                _POPPLER_FREE(obj1);
            }
              haveColorKeyMask = gTrue;
        } else if (maskObj.isStream()) {
            // explicit mask
            if (inlineImg) {
                goto err1;
            }
            maskStr = maskObj.getStream();
            maskDict = maskObj.getStream()->getDict();
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Width");
            if (obj1.isNull()) {
                _POPPLER_FREE(obj1);
                _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "W");
            }
            if (!obj1.isInt()) {
                goto err2;
            }
            maskWidth = obj1.getInt();
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Height");
            if (obj1.isNull()) {
                _POPPLER_FREE(obj1);
                _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "H");
            }
            if (!obj1.isInt()) {
                goto err2;
            }
            maskHeight = obj1.getInt();
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "ImageMask");
            if (obj1.isNull()) {
                _POPPLER_FREE(obj1);
                _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "IM");
            }
            if (!obj1.isBool() || !obj1.getBool()) {
                goto err2;
            }
            _POPPLER_FREE(obj1);
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Interpolate");
	    if (obj1.isNull()) {
	      _POPPLER_FREE(obj1);
	      _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "I");
	    }
	    if (obj1.isBool())
	      maskInterpolate = obj1.getBool();
	    else
	      maskInterpolate = gFalse;
	    _POPPLER_FREE(obj1);
            maskInvert = gFalse;
            _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "Decode");
            if (obj1.isNull()) {
                _POPPLER_FREE(obj1);
                _POPPLER_CALL_ARGS(obj1, maskDict->lookup, "D");
            }
            if (obj1.isArray()) {
                _POPPLER_CALL_ARGS(obj2, obj1.getArray()->get, 0);
                if (obj2.isInt() && obj2.getInt() == 1) {
                    maskInvert = gTrue;
                }
                _POPPLER_FREE(obj2);
            } else if (!obj1.isNull()) {
                goto err2;
            }
            _POPPLER_FREE(obj1);
            haveExplicitMask = gTrue;
        }
        
        // draw it
        if (haveSoftMask) {
	    builder->addSoftMaskedImage(state, str, width, height, colorMap.get(), interpolate,
				maskStr, maskWidth, maskHeight, maskColorMap.get(), maskInterpolate);
        } else if (haveExplicitMask) {
 	    builder->addMaskedImage(state, str, width, height, colorMap.get(), interpolate,
				maskStr, maskWidth, maskHeight, maskInvert, maskInterpolate);
        } else {
	    builder->addImage(state, str, width, height, colorMap.get(), interpolate,
		        haveColorKeyMask ? maskColors : nullptr);
        }
        
        _POPPLER_FREE(maskObj);
        _POPPLER_FREE(smaskObj);
    }

    return;

 err2:
    _POPPLER_FREE(obj1);
 err1:
    error(errSyntaxError, getPos(), "Bad image parameters");
}

void PdfParser::doForm(Object *str, double *offset)
{
    Dict *dict;
    GBool transpGroup, isolated, knockout;
    Object matrixObj, bboxObj;
    double m[6], bbox[4];
    Object resObj;
    Dict *resDict;
    Object obj1, obj2, obj3;
    int i;

    // check for excessive recursion
    if (formDepth > 20) {
        return;
    }

    // get stream dict
    dict = str->getStream()->getDict();

    // check form type
    _POPPLER_CALL_ARGS(obj1, dict->lookup, "FormType");
    if (!(obj1.isNull() || (obj1.isInt() && obj1.getInt() == 1))) {
        error(errSyntaxError, getPos(), "Unknown form type");
    }
    _POPPLER_FREE(obj1);

    // get bounding box
    _POPPLER_CALL_ARGS(bboxObj, dict->lookup, "BBox");
    if (!bboxObj.isArray()) {
        _POPPLER_FREE(bboxObj);
        error(errSyntaxError, getPos(), "Bad form bounding box");
        return;
    }
    for (i = 0; i < 4; ++i) {
        _POPPLER_CALL_ARGS(obj1, bboxObj.getArray()->get, i);
        bbox[i] = obj1.getNum();
        _POPPLER_FREE(obj1);
    }
    _POPPLER_FREE(bboxObj);

    // get matrix
    _POPPLER_CALL_ARGS(matrixObj, dict->lookup, "Matrix");
    if (matrixObj.isArray()) {
        for (i = 0; i < 6; ++i) {
        _POPPLER_CALL_ARGS(obj1, matrixObj.getArray()->get, i);
        m[i] = obj1.getNum();
        _POPPLER_FREE(obj1);
        }
    } else {
        m[0] = 1;
        m[1] = 0;
        m[2] = 0;
        m[3] = 1;
        m[4] = 0;
        m[5] = 0;
    }
    _POPPLER_FREE(matrixObj);

    if (offset) {
        m[4] += offset[0];
        m[5] += offset[1];
    }

    // get resources
    _POPPLER_CALL_ARGS(resObj, dict->lookup, "Resources");
    resDict = resObj.isDict() ? resObj.getDict() : (Dict *)nullptr;

    // check for a transparency group
    transpGroup = isolated = knockout = gFalse;
    std::unique_ptr<GfxColorSpace> blendingColorSpace;
    if (_POPPLER_CALL_ARGS_DEREF(obj1, dict->lookup, "Group").isDict()) {
        if (_POPPLER_CALL_ARGS_DEREF(obj2, obj1.dictLookup, "S").isName("Transparency")) {
        transpGroup = gTrue;
        if (!_POPPLER_CALL_ARGS_DEREF(obj3, obj1.dictLookup, "CS").isNull()) {
            blendingColorSpace = std::unique_ptr<GfxColorSpace>(GfxColorSpace::parse(nullptr, &obj3, nullptr, state));
        }
        _POPPLER_FREE(obj3);
        if (_POPPLER_CALL_ARGS_DEREF(obj3, obj1.dictLookup, "I").isBool()) {
                isolated = obj3.getBool();
        }
        _POPPLER_FREE(obj3);
        if (_POPPLER_CALL_ARGS_DEREF(obj3, obj1.dictLookup, "K").isBool()) {
                knockout = obj3.getBool();
        }
        _POPPLER_FREE(obj3);
        }
        _POPPLER_FREE(obj2);
    }
    _POPPLER_FREE(obj1);

    // draw it
    doForm1(str, resDict, m, bbox, transpGroup, gFalse, blendingColorSpace.get(), isolated, knockout);

    _POPPLER_FREE(resObj);
}

void PdfParser::doForm1(Object *str, Dict *resDict, double *matrix, double *bbox, GBool transpGroup, GBool softMask,
                        GfxColorSpace *blendingColorSpace, GBool isolated, GBool knockout, GBool alpha,
                        Function *transferFunc, GfxColor *backdropColor)
{
    formDepth++;

    Parser *oldParser;

    // push new resources on stack
    pushResources(resDict);

    // set up clipping groups, letting builder handle SVG group creation
    builder->startGroup(state, bbox, blendingColorSpace, isolated, knockout, softMask);

    // save current graphics state
    saveState();

    // kill any pre-existing path
    state->clearPath();

    // save current parser
    oldParser = parser;

    // set form transformation matrix
    state->concatCTM(matrix[0], matrix[1], matrix[2], matrix[3], matrix[4], matrix[5]);

    // set form bounding box
    state->moveTo(bbox[0], bbox[1]);
    state->lineTo(bbox[2], bbox[1]);
    state->lineTo(bbox[2], bbox[3]);
    state->lineTo(bbox[0], bbox[3]);
    state->closePath();
    state->clip();
    builder->setClip(state, clipNormal, true);
    state->clearPath();

    if (softMask || transpGroup) {
        if (state->getBlendMode() != gfxBlendNormal) {
            state->setBlendMode(gfxBlendNormal);
        }
        if (state->getFillOpacity() != 1) {
            builder->setGroupOpacity(state->getFillOpacity());
            state->setFillOpacity(1);
        }
        if (state->getStrokeOpacity() != 1) {
            state->setStrokeOpacity(1);
        }
    }

    // set new base matrix
    auto oldBaseMatrix = baseMatrix;
    baseMatrix = stateToAffine(state);
    builder->setBaseMatrix(baseMatrix);

    // draw the form
    parse(str, gFalse);

    // restore base matrix
    baseMatrix = oldBaseMatrix;
    builder->setBaseMatrix(baseMatrix);

    // restore parser
    parser = oldParser;

    // restore graphics state
    restoreState();

    // pop resource stack
    popResources();    

    // complete any masking
    builder->finishGroup(state, softMask);
    formDepth--;
}

//------------------------------------------------------------------------
// in-line image operators
//------------------------------------------------------------------------

void PdfParser::opBeginImage(Object /*args*/[], int /*numArgs*/)
{
  // build dict/stream
  Stream *str = buildImageStream();

  // display the image
  if (str) {
    doImage(nullptr, str, gTrue);
  
    // skip 'EI' tag
    int c1 = str->getUndecodedStream()->getChar();
    int c2 = str->getUndecodedStream()->getChar();
    while (!(c1 == 'E' && c2 == 'I') && c2 != EOF) {
      c1 = c2;
      c2 = str->getUndecodedStream()->getChar();
    }
    delete str;
  }
}

Stream *PdfParser::buildImageStream() {
  Object dict;
  Object obj;
  Stream *str;

  // build dictionary
#if defined(POPPLER_NEW_OBJECT_API)
#if POPPLER_CHECK_VERSION(26, 3, 0)
  dict = Object(std::make_unique<Dict>(xref));
#else
  dict = Object(new Dict(xref));
#endif
#else
  dict.initDict(xref);
#endif
  _POPPLER_CALL(obj, parser->getObj);
  while (!obj.isCmd(const_cast<char*>("ID")) && !obj.isEOF()) {
    if (!obj.isName()) {
      error(errSyntaxError, getPos(), "Inline image dictionary key must be a name object");
      _POPPLER_FREE(obj);
    } else {
      Object obj2;
      _POPPLER_CALL(obj2, parser->getObj);
      if (obj2.isEOF() || obj2.isError()) {
        _POPPLER_FREE(obj);
	break;
      }
      _POPPLER_DICTADD(dict, obj.getName(), obj2);
      _POPPLER_FREE(obj);
      _POPPLER_FREE(obj2);
    }
    _POPPLER_CALL(obj, parser->getObj);
  }
  if (obj.isEOF()) {
    error(errSyntaxError, getPos(), "End of file in inline image");
    _POPPLER_FREE(obj);
    _POPPLER_FREE(dict);
    return nullptr;
  }
  _POPPLER_FREE(obj);

  // make stream
#if defined(POPPLER_NEW_OBJECT_API)
  str = new EmbedStream(parser->getStream(), dict.copy(), gFalse, 0);
#if POPPLER_CHECK_VERSION(26, 2, 0)
  str = str->addFilters(std::unique_ptr<Stream>(str), dict.getDict()).release();
#else
  str = str->addFilters(dict.getDict());
#endif
#else
  str = new EmbedStream(parser->getStream(), &dict, gFalse, 0);
  str = str->addFilters(&dict);
#endif

  return str;
}

void PdfParser::opImageData(Object /*args*/[], int /*numArgs*/)
{
  error(errInternal, getPos(), "Internal: got 'ID' operator");
}

void PdfParser::opEndImage(Object /*args*/[], int /*numArgs*/)
{
  error(errInternal, getPos(), "Internal: got 'EI' operator");
}

//------------------------------------------------------------------------
// type 3 font operators
//------------------------------------------------------------------------

void PdfParser::opSetCharWidth(Object /*args*/[], int /*numArgs*/)
{
}

void PdfParser::opSetCacheDevice(Object /*args*/[], int /*numArgs*/)
{
}

//------------------------------------------------------------------------
// compatibility operators
//------------------------------------------------------------------------

void PdfParser::opBeginIgnoreUndef(Object /*args*/[], int /*numArgs*/)
{
  ++ignoreUndef;
}

void PdfParser::opEndIgnoreUndef(Object /*args*/[], int /*numArgs*/)
{
  if (ignoreUndef > 0)
    --ignoreUndef;
}

//------------------------------------------------------------------------
// marked content operators
//------------------------------------------------------------------------

void PdfParser::opBeginMarkedContent(Object args[], int numArgs) {
    if (ignoreMarkedContent()) {
        return;
    }

    if (printCommands) {
        printf("  marked content: %s ", args[0].getName());
        if (numArgs == 2)
            args[2].print(stdout);
        printf("\n");
        fflush(stdout);
    }
    if (numArgs == 2 && args[1].isName()) {
        // Optional content (OC) to add objects to layer.
        builder->beginMarkedContent(args[0].getName(), args[1].getName());
    } else {
        builder->beginMarkedContent();
    }
}

void PdfParser::opEndMarkedContent(Object /*args*/[], int /*numArgs*/)
{
    if (ignoreMarkedContent()) {
        return;
    }
    builder->endMarkedContent();
}

/*
  Decide whether to ignore marked content commands based on selected
  group handling mode and form depth.
*/
bool PdfParser::ignoreMarkedContent()
{
    auto group_by = builder->getGroupBy();
    return group_by == GroupBy::BY_XOBJECT && formDepth != 0;
}

void PdfParser::opMarkPoint(Object args[], int numArgs) {
  if (printCommands) {
    printf("  mark point: %s ", args[0].getName());
    if (numArgs == 2)
      args[2].print(stdout);
    printf("\n");
    fflush(stdout);
  }

  if(numArgs == 2) {
    //out->markPoint(args[0].getName(),args[1].getDict());
  } else {
    //out->markPoint(args[0].getName());
  }

}

//------------------------------------------------------------------------
// misc
//------------------------------------------------------------------------

void PdfParser::saveState() {
    bool is_radial = false;
    GfxPattern *pattern = state->getFillPattern();

    if (pattern && pattern->getType() == 2) {
        GfxShadingPattern *shading_pattern = static_cast<GfxShadingPattern *>(pattern);
        GfxShading *shading = shading_pattern->getShading();
        if (shading->getType() == 3)
            is_radial = true;
    }

    if (is_radial)
        state->save(); // nasty hack to prevent GfxRadialShading from getting corrupted during copy operation
    else
        state = state->save(); // see LP Bug 919176 comment 8
    builder->saveState(state);
}

void PdfParser::restoreState() {
    builder->restoreState(state);
    state = state->restore();
}

void PdfParser::pushResources(Dict *resDict) {
  res = new GfxResources(xref, resDict, res);
}

void PdfParser::popResources() {
  GfxResources *resPtr;

  resPtr = res->getNext();
  delete res;
  res = resPtr;
}

void PdfParser::setDefaultApproximationPrecision() {
  setApproximationPrecision(defaultShadingColorDelta, defaultShadingMaxDepth);
}

void PdfParser::setApproximationPrecision(double colorDelta, int maxDepth) {
  this->colorDelta = dblToCol(colorDelta);
  this->maxDepth = maxDepth;
  // Might need to be tweaked somewhat, but the finest value somewhat smaller
  // with the value #defined in poppler/Gfx.cc as 5e-3
  gouraudParameterizedColorDelta = colorDelta;
}

/**
 * Optional content groups are often used in ai files, but
 * not always and can be useful ways of collecting objects.
 */
void PdfParser::loadOptionalContentLayers(Dict *resources)
{
    if (!resources)
        return;

    auto props = resources->lookup("Properties");
    auto cat = _pdf_doc->getCatalog();
    auto ocgs = cat->getOptContentConfig();

    // map from page-level OCG names (e.g. MC0, MC1) to layer names
    if (props.isDict() && ocgs) {
        auto dict = props.getDict();

        for (auto j = 0; j < dict->getLength(); j++) {
            auto val = dict->getVal(j);
            if (!val.isDict())
                continue;
            auto dict2 = val.getDict();
            if (dict2->lookup("Type").isName("OCG")) {
                std::string label = getDictString(dict2, "Name");
                auto visible = true;
                // Normally we'd use poppler optContentIsVisible, but these dict
                // objects don't retain their references so can't be used directly.
#if POPPLER_CHECK_VERSION(26, 2, 0)
                for (auto &[ref, ocg] : ocgs->getOCGs()) {
                    if (ocg->getName()->toStr() == label)
                        visible = ocg->getState() == OptionalContentGroup::On;
                }
#else
                for (auto &[ref, ocg] : ocgs->getOCGs()) {
                    if (ocg->getName()->cmp(label) == 0)
                        visible = ocg->getState() == OptionalContentGroup::On;
                }
#endif
                builder->addOptionalGroup(dict->getKey(j), label, visible);
            }
        }
    } else if (ocgs) {
        // OCGs defined in the document root, but not mapped at the page level.
        // Add them to the builder with arbitrary names. Order doesn't really matter, as they don't
        // get created in the SVG until encountered in the content stream.
        int layer = 1;
        for (auto &[ref, ocg] : ocgs->getOCGs()) {
            auto key = "OC" + std::to_string(layer++);
            builder->addOptionalGroup(key, ocg->getName()->c_str(), ocg->getState() == OptionalContentGroup::On);
        }
    }

    // if top-level groups are by OCGs, recurse in case nested objects have OCGs
    if (builder->getGroupBy() == GroupBy::BY_OCGS) {
        auto xobjects = resources->lookup("XObject");
        if (xobjects.isDict()) {
            auto dict = xobjects.getDict();
            for (auto i = 0; i < dict->getLength(); ++i) {
                auto xobj = dict->getVal(i);
                if (xobj.isStream()) {
                    if (xobj.getStream()->getDict()->lookup("Subtype").isName("Form")) {
                        auto form_resources = xobj.getStream()->getDict()->lookup("Resources");
                        if (form_resources.isDict()) {
                            // phew, that's a lot of nesting
                            loadOptionalContentLayers(form_resources.getDict());
                        }
                    }
                }
            }
        }
    }
}

/**
 * Load the internal ICC profile from the PDF file.
 */
void PdfParser::loadColorProfile()
{
    Object catDict = xref->getCatalog();
    if (!catDict.isDict())
        return;

    Object outputIntents = catDict.dictLookup("OutputIntents");
    if (!outputIntents.isArray() || outputIntents.getArray()->getLength() != 1)
        return;

    Object firstElement = outputIntents.getArray()->get(0);
    if (!firstElement.isDict())
        return;

    Object profile = firstElement.dictLookup("DestOutputProfile");
    if (!profile.isStream())
        return;

    Stream *iccStream = profile.getStream();
#if POPPLER_CHECK_VERSION(22, 4, 0)
    std::vector<unsigned char> profBuf = iccStream->toUnsignedChars(65536, 65536);
    builder->addColorProfile(profBuf.data(), profBuf.size());
#else
    int length = 0;
    unsigned char *profBuf = iccStream->toUnsignedChars(&length, 65536, 65536);
    builder->addColorProfile(profBuf, length);
#endif
}

void PdfParser::build_annots(const Object &annot, int page_num)
{
    Object AP_obj, N_obj, Rect_obj, xy_obj, first_state_obj;
    double offset[2];
    Dict *annot_dict;

    if (!annot.isDict())
        return;
    annot_dict = annot.getDict();

    _POPPLER_CALL_ARGS(AP_obj, annot_dict->lookup, "AP");
    // If AP stream is present we use it
    if (AP_obj.isDict()) {
        _POPPLER_CALL_ARGS(N_obj, AP_obj.getDict()->lookup, "N");
        if (N_obj.isDict()) {
            // If there are several appearance states, we draw the first one
            _POPPLER_CALL_ARGS(first_state_obj, N_obj.getDict()->getVal, 0);
        } else {
            // If there is only one appearance state, we get directly the stream
            first_state_obj = N_obj.copy();
        }
        if (first_state_obj.isStream()) {
            // even though these aren't defined in OCProperties, add them to the ocgs map of the builder
            auto annot_label = std::to_string(page_num) + " - Annotations";
            auto annot_group = "A" + std::to_string(page_num);  
            builder->addOptionalGroup(annot_group, annot_label);
            builder->beginXObjectLayer(annot_label);
            _POPPLER_CALL_ARGS(Rect_obj, annot_dict->lookup, "Rect");
            if (Rect_obj.isArray()) {
                for (int i = 0; i < 2; i++) {
                    _POPPLER_CALL_ARGS(xy_obj, Rect_obj.getArray()->get, i);
                    offset[i] = xy_obj.getNum();
                }
                doForm(&first_state_obj, offset);
            }
            builder->endMarkedContent();
        }
        _POPPLER_FREE(AP_obj);
        _POPPLER_FREE(N_obj);
        _POPPLER_FREE(Rect_obj);
        _POPPLER_FREE(xy_obj);
        _POPPLER_FREE(first_state_obj);
    } else {
        // No AP stream, we need to implement a Inkscape annotation handler for annot type
        error(errInternal, -1, "No inkscape handler for this annotation type");
    }
}

/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim:filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99:
