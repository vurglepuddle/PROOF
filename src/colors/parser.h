// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 AUTHORS
 *
 * Released under GNU GPL v2+, read the file 'COPYING' for more information.
 */
#ifndef SEEN_COLORS_PARSING_H
#define SEEN_COLORS_PARSING_H

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "spaces/enum.h"

namespace Inkscape::Colors {

// Escape old XML-style ICC identifiers before feeding CSS to libcroco.
// The CSS parser decodes the escapes, retaining the original profile lookup name.
std::string escape_legacy_icc_names(std::string const &css);

class Parser
{
public:
    Parser(std::string prefix, Space::Type type)
        : _prefix(std::move(prefix))
        , _type(type)
    {}
    virtual ~Parser() = default;

    Space::Type getType() const { return _type; }
    std::string const getPrefix() const { return _prefix; }
    static std::string getCssPrefix(std::istringstream &ss);
    static bool css_number(std::istringstream &ss, double &value, std::string &unit, bool &end, char const sep = 0x0);
    static bool append_css_value(std::istringstream &ss, std::vector<double> &output, bool &end, char const sep = 0x0,
                                 double scale = 1.0, double pc_scale = 100.0);

    // TODO message: return reason for parsing failure. Otherwise we cannot tell the user why we reject certain colors.
    virtual bool parse(std::istringstream &ss, std::vector<double> &output) const { return false; };
    virtual bool parse(std::istringstream &ss, std::vector<double> &output, bool &more) const
    {
        return parse(ss, output);
    };
    virtual std::string parseColor(std::istringstream &ss, std::vector<double> &output, bool &more) const;

private:
    std::string _prefix;
    Space::Type _type;
};

class LegacyParser : public Parser
{
public:
    LegacyParser(std::string const &prefix, Space::Type type, bool alpha)
        : Parser(alpha ? prefix + "a" : prefix, type)
        , _alpha(alpha)
    {}

protected:
    bool _alpha = false;
};

class HueParser : public LegacyParser
{
public:
    HueParser(std::string const &prefix, Space::Type type, bool alpha, double scale = 1.0)
        : LegacyParser(prefix, type, alpha)
	, _scale(scale)
    {}
    bool parse(std::istringstream &ss, std::vector<double> &output) const override;
protected:
    double _scale = 1.0;
};

class HexParser : public Parser
{
public:
    HexParser()
        : Parser("#", Space::Type::RGB)
    {}
    bool parse(std::istringstream &input, std::vector<double> &output, bool &more) const override;
};

class CssParser : public Parser
{
public:
    CssParser(std::string prefix, Space::Type type, unsigned int channels)
        : Parser(prefix, type)
        , _channels(channels)
    {}

private:
    bool parse(std::istringstream &ss, std::vector<double> &output) const override;

    unsigned int _channels;
};

class Parsers
{
private:
    Parsers(Parsers const &) = delete;
    void operator=(Parsers const &) = delete;

public:
    Parsers();
    ~Parsers() = default;

    static Parsers &get()
    {
        static Parsers instance;
        return instance;
    }

    bool parse(std::string const &input, Space::Type &type, std::string &cms, std::vector<double> &values,
               std::vector<double> &fallback) const;

protected:
    friend class Manager;

    void addParser(Parser *parser);

private:
    bool _parse(std::istringstream &ss, Space::Type &type, std::string &cms, std::vector<double> &values,
                std::vector<double> &fallback) const;

    std::map<std::string, std::vector<std::shared_ptr<Parser>>> _parsers;
};

} // namespace Inkscape::Colors

#endif // SEEN_COLORS_PARSING_H
