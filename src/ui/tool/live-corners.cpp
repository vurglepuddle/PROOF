// SPDX-License-Identifier: GPL-2.0-or-later
#include "live-corners.h"

#include <2geom/bezier-curve.h>
#include <2geom/line.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <memory>
#include <sstream>

#include "helper/geom.h"
#include "svg/svg.h"

namespace Inkscape::UI::LiveCorners {
namespace {
constexpr double epsilon = 1e-7;

std::size_t count(Geom::Path const &path)
{
    auto n = path.size_default();
    if (path.closed() && n && Geom::are_near(path[n - 1].initialPoint(), path[n - 1].finalPoint(), epsilon)) --n;
    return n;
}

double timeAtDistance(Geom::Curve const &curve, double distance, bool from_end)
{
    auto length = curve.length();
    if (distance <= epsilon) return from_end ? 1 : 0;
    if (distance >= length) return from_end ? 0 : 1;
    double low = 0, high = 1;
    for (int i = 0; i < 35; ++i) {
        double mid = (low + high) / 2;
        std::unique_ptr<Geom::Curve> portion(curve.portion(0, mid));
        double target = from_end ? length - distance : distance;
        if (portion->length() < target) low = mid;
        else high = mid;
    }
    return (low + high) / 2;
}

bool same(Geom::Curve const &a, Geom::Curve const &b)
{
    for (double t : {0., .25, .5, .75, 1.}) {
        if (!Geom::are_near(a.pointAt(t), b.pointAt(t), 1e-5)) return false;
    }
    return true;
}

bool same(Geom::PathVector const &a, Geom::PathVector const &b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t p = 0; p < a.size(); ++p) {
        if (a[p].closed() != b[p].closed() || count(a[p]) != count(b[p])) return false;
        for (std::size_t i = 0; i < count(a[p]); ++i) {
            if (!same(a[p][i], b[p][i])) return false;
        }
    }
    return true;
}

std::vector<std::string> split(std::string const &value, char separator)
{
    std::vector<std::string> parts;
    std::istringstream stream(value);
    for (std::string part; std::getline(stream, part, separator);) parts.push_back(std::move(part));
    return parts;
}
} // namespace

Model Model::fromPath(Geom::PathVector const &path)
{
    Model result;
    result.source = pathv_to_linear_and_cubic_beziers(path);
    for (std::size_t p = 0; p < result.source.size(); ++p) {
        auto const &subpath = result.source[p];
        for (std::size_t node = subpath.closed() ? 0 : 1; node < count(subpath); ++node) {
            Corner corner{p, node};
            if (result.geometry(corner)) result.corners.push_back(corner);
        }
    }
    return result;
}

std::optional<Geometry> Model::geometry(Corner const &corner) const
{
    if (corner.path >= source.size()) return {};
    auto const &path = source[corner.path];
    auto n = count(path);
    if (n < 2 || corner.node >= n || (!path.closed() && corner.node == 0)) return {};
    auto const &incoming = path[(corner.node + n - 1) % n];
    auto const &outgoing = path[corner.node];
    if (incoming.length() < epsilon || outgoing.length() < epsilon) return {};
    auto a = -incoming.unitTangentAt(1);
    auto b = outgoing.unitTangentAt(0);
    double angle = std::acos(std::clamp(Geom::dot(a, b), -1., 1.));
    // Endpoints and smooth/straight junctions have no corner to round.
    if (!std::isfinite(angle) || angle < .01 || angle > M_PI - .01) return {};
    auto direction = a + b;
    if (Geom::L2(direction) < epsilon) return {};
    return Geometry{outgoing.initialPoint(), Geom::unit_vector(direction), angle,
                    std::min(incoming.length(), outgoing.length()) * std::tan(angle / 2)};
}

std::optional<double> Model::displayAngle(Corner const &corner, Geom::Affine const &transform) const
{
    if (!geometry(corner)) return {};
    auto const &path = source[corner.path];
    auto const n = count(path);
    auto const &incoming = path[(corner.node + n - 1) % n];
    auto const &outgoing = path[corner.node];
    auto anchor = outgoing.initialPoint();
    auto at = anchor * transform;
    auto a = (anchor - incoming.unitTangentAt(1)) * transform - at;
    auto b = (anchor + outgoing.unitTangentAt(0)) * transform - at;
    auto const length_a = Geom::L2(a), length_b = Geom::L2(b);
    if (!std::isfinite(length_a) || !std::isfinite(length_b) || length_a < epsilon || length_b < epsilon) return {};
    return std::acos(std::clamp(Geom::dot(a / length_a, b / length_b), -1., 1.));
}

Model::Rendered Model::build() const
{
    Rendered result;
    for (std::size_t p = 0; p < source.size(); ++p) {
        auto const &path = source[p];
        auto n = count(path);
        if (!n) {
            result.path.push_back(path);
            result.pieces.emplace_back();
            continue;
        }
        std::vector<double> distances(n, 0);
        std::vector<std::optional<std::size_t>> index(n);
        for (std::size_t c = 0; c < corners.size(); ++c) {
            auto const &corner = corners[c];
            if (corner.path != p || corner.node >= n || corner.radius <= epsilon) continue;
            if (auto info = geometry(corner)) {
                // Relative specifies equal distance along the sides at a given
                // value; Absolute specifies the same circle radius at every angle.
                distances[corner.node] = corner.rounding == Rounding::Relative
                    ? corner.radius : corner.radius / std::tan(info->angle / 2);
                distances[corner.node] = std::min(distances[corner.node],
                    info->maximum / std::tan(info->angle / 2));
                index[corner.node] = c;
            }
        }
        // Adjacent corners share a segment. Reduce both proportionately, then
        // use the smaller budget on both sides of each individual corner.
        for (std::size_t i = 0; i < n; ++i) {
            auto next = (i + 1) % n;
            double end = (!path.closed() && i + 1 == n) ? 0 : distances[next];
            double requested = distances[i] + end;
            double available = path[i].length() * (1 - 1e-6);
            if (requested > available && requested > epsilon) {
                double factor = available / requested;
                distances[i] *= factor;
                if (path.closed() || i + 1 < n) distances[next] *= factor;
            }
        }
        Geom::Path output;
        std::vector<Piece> pieces;
        for (std::size_t i = 0; i < n; ++i) {
            auto next = (i + 1) % n;
            double from = timeAtDistance(path[i], distances[i], false);
            double end_distance = (!path.closed() && i + 1 == n) ? 0 : distances[next];
            double to = timeAtDistance(path[i], end_distance, true);
            to = std::max(from, to);
            std::unique_ptr<Geom::Curve> kept(path[i].portion(from, to));
            if (i == 0) output.start(kept->initialPoint());
            else kept->setInitial(output.finalPoint());
            output.append(*kept);
            pieces.push_back(Piece{i, {}, from, to});
            if (!index[next] || end_distance <= epsilon || (!path.closed() && i + 1 == n)) continue;
            auto const &corner = corners[*index[next]];
            auto const &following = path[next];
            auto end_t = timeAtDistance(following, distances[next], false);
            auto start = output.finalPoint(), end = following.pointAt(end_t);
            if (corner.style == Style::Chamfer) {
                output.appendNew<Geom::LineSegment>(end);
            } else {
                auto tangent0 = path[i].unitTangentAt(to);
                auto tangent1 = following.unitTangentAt(end_t);
                double turn = std::acos(std::clamp(Geom::dot(tangent0, tangent1), -1., 1.));
                double chord = Geom::distance(start, end);
                double r = chord / std::max(2 * std::sin(turn / 2), epsilon);
                double handle = 4. / 3. * r * std::tan(turn / 4) * corner.handle_factor;
                auto h0 = start + tangent0 * handle, h1 = end - tangent1 * handle;
                if (corner.style == Style::Inverted) {
                    // Reflect the round's controls through its chord. This is
                    // the inverse fillet, with the same endpoints and amount.
                    auto axis = Geom::unit_vector(end - start);
                    auto reflect = [&](Geom::Point const &q) {
                        auto delta = q - start;
                        return start + 2 * Geom::dot(delta, axis) * axis - delta;
                    };
                    h0 = reflect(h0);
                    h1 = reflect(h1);
                }
                output.appendNew<Geom::CubicBezier>(h0, h1, end);
            }
            pieces.push_back(Piece{i, index[next]});
        }
        output.close(path.closed());
        result.path.push_back(std::move(output));
        result.pieces.push_back(std::move(pieces));
    }
    result.path *= frame;
    return result;
}

Geom::PathVector Model::render() const { return build().path; }

Geom::PathVector Model::roundedCurves(std::vector<std::size_t> const &selected) const
{
    auto rendered = build();
    Geom::PathVector result;
    for (std::size_t p = 0; p < rendered.pieces.size(); ++p) {
        for (std::size_t i = 0; i < rendered.pieces[p].size(); ++i) {
            auto corner = rendered.pieces[p][i].corner;
            if (!corner || std::find(selected.begin(), selected.end(), *corner) == selected.end()) continue;
            auto const &curve = rendered.path[p][i];
            Geom::Path path(curve.initialPoint());
            path.append(curve);
            result.push_back(std::move(path));
        }
    }
    return result;
}

std::vector<std::vector<Geom::Point>> Model::nodePoints() const
{
    std::vector<std::vector<Geom::Point>> points(corners.size());
    for (std::size_t c = 0; c < corners.size(); ++c) {
        if (auto info = geometry(corners[c])) points[c].push_back(info->anchor * frame);
    }
    auto rendered = build();
    for (std::size_t p = 0; p < rendered.pieces.size(); ++p) {
        for (std::size_t i = 0; i < rendered.pieces[p].size(); ++i) {
            if (auto c = rendered.pieces[p][i].corner) {
                points[*c] = {rendered.path[p][i].initialPoint(), rendered.path[p][i].finalPoint()};
            }
        }
    }
    return points;
}

bool Model::hasRounding() const
{
    return std::any_of(corners.begin(), corners.end(), [](auto const &c) { return c.radius > epsilon; });
}

double Model::setRadius(std::vector<std::size_t> const &selected, double requested)
{
    if (!std::isfinite(requested)) return 0;
    double maximum = std::max(0., requested);
    for (std::size_t p = 0; p < source.size(); ++p) {
        auto const &path = source[p];
        auto n = count(path);
        for (std::size_t i = 0; i < n; ++i) {
            double coefficient = 0, occupied = 0;
            for (std::size_t c = 0; c < corners.size(); ++c) {
                auto const &corner = corners[c];
                if (corner.path != p || (corner.node != i &&
                    (corner.node != (i + 1) % n || (!path.closed() && i + 1 == n)))) continue;
                auto info = geometry(corner);
                if (!info) continue;
                double factor = corner.rounding == Rounding::Relative ? 1 : 1 / std::tan(info->angle / 2);
                if (std::find(selected.begin(), selected.end(), c) != selected.end()) coefficient += factor;
                else occupied += corner.radius * factor;
            }
            if (coefficient > 0) maximum = std::min(maximum,
                std::max(0., path[i].length() * (1 - 1e-6) - occupied) / coefficient);
        }
    }
    for (auto c : selected) if (c < corners.size()) corners[c].radius = maximum;
    return maximum;
}

bool Model::reconcile(Geom::PathVector const &edited)
{
    auto rendered = build();
    if (same(rendered.path, edited)) return true;
    if (edited.size() != rendered.path.size()) return reconcileTopology(edited);
    std::vector<Geom::Point> before, after;
    for (std::size_t p = 0; p < edited.size(); ++p) {
        if (count(edited[p]) != rendered.pieces[p].size() || edited[p].closed() != source[p].closed()) return reconcileTopology(edited);
        for (std::size_t i = 0; i < count(edited[p]); ++i) {
            for (double t : {0., .5, 1.}) {
                before.push_back(rendered.path[p][i].pointAt(t));
                after.push_back(edited[p][i].pointAt(t));
            }
        }
    }
    // Fit an object transform, then require every curve to agree with it.
    // Keeping it as a frame also preserves nonuniformly scaled round geometry.
    if (before.size() >= 3) {
        std::size_t b = 1;
        while (b < before.size() && Geom::are_near(before[0], before[b], epsilon)) ++b;
        if (b < before.size()) {
            std::size_t c = b + 1;
            while (c < before.size() && std::abs(Geom::cross(before[b] - before[0], before[c] - before[0])) < epsilon) ++c;
            if (c < before.size()) {
                auto basis = [](Geom::Point const &a, Geom::Point const &b, Geom::Point const &c) {
                    return Geom::Affine(b[0] - a[0], b[1] - a[1], c[0] - a[0], c[1] - a[1], a[0], a[1]);
                };
                auto transform = basis(before[0], before[b], before[c]).inverse() * basis(after[0], after[b], after[c]);
                auto transformed = rendered.path;
                transformed *= transform;
                if (std::abs(transform.det()) > epsilon && same(transformed, edited)) {
                    frame *= transform;
                    return true;
                }
            }
        }
    }
    auto local = edited;
    local *= frame.inverse();
    rendered.path *= frame.inverse();
    std::vector<bool> valid(corners.size(), true);
    for (std::size_t p = 0; p < local.size(); ++p) {
        for (std::size_t i = 0; i < rendered.pieces[p].size(); ++i) {
            if (same(local[p][i], rendered.path[p][i])) continue;
            auto const &piece = rendered.pieces[p][i];
            for (std::size_t c = 0; c < corners.size(); ++c) {
                auto const &corner = corners[c];
                auto n = count(source[p]);
                if (corner.path == p && (piece.corner == c || (!piece.corner &&
                    (corner.node == piece.source_curve || corner.node == (piece.source_curve + 1) % n)))) valid[c] = false;
            }
        }
    }
    Model updated;
    updated.frame = frame;
    for (std::size_t p = 0; p < local.size(); ++p) {
        Geom::Path base;
        bool started = false;
        for (std::size_t i = 0; i < rendered.pieces[p].size(); ++i) {
            auto const &piece = rendered.pieces[p][i];
            if (piece.corner && valid[*piece.corner]) continue;
            std::unique_ptr<Geom::Curve> curve;
            if (piece.corner || !same(local[p][i], rendered.path[p][i])) curve.reset(local[p][i].duplicate());
            else {
                double from = piece.from, to = piece.to;
                auto n = count(source[p]);
                for (std::size_t c = 0; c < corners.size(); ++c) {
                    if (!valid[c] || corners[c].path != p) continue;
                    if (corners[c].node == piece.source_curve) from = 0;
                    if (corners[c].node == (piece.source_curve + 1) % n) to = 1;
                }
                curve.reset(source[p][piece.source_curve].portion(from, to));
            }
            if (!started) { base.start(curve->initialPoint()); started = true; }
            else curve->setInitial(base.finalPoint());
            // A surviving corner lies at the start of this source segment.
            if (!piece.corner) {
                for (std::size_t c = 0; c < corners.size(); ++c) {
                    if (valid[c] && corners[c].path == p && corners[c].node == piece.source_curve) {
                        auto corner = corners[c];
                        corner.node = base.size_open();
                        updated.corners.push_back(corner);
                    }
                }
            }
            base.append(*curve);
        }
        base.close(local[p].closed());
        updated.source.push_back(std::move(base));
    }
    // New ordinary cusp anchors may be rounded again. Avoid duplicate entries.
    auto fresh = fromPath(updated.source);
    for (auto const &corner : fresh.corners) {
        if (std::none_of(updated.corners.begin(), updated.corners.end(), [&](auto const &c) {
            return c.path == corner.path && c.node == corner.node;
        })) updated.corners.push_back(corner);
    }
    if (!same(updated.render(), edited)) return reconcileTopology(edited);
    *this = std::move(updated);
    return true;
}

bool Model::reconcileTopology(Geom::PathVector const &edited)
{
    // Match the actual rounded arc and its immediately adjoining portions.
    // Inserting an anchor on a side or changing another subpath must not erase
    // unrelated live corners. Only portions of the original side can be untrimmed.
    auto local = pathv_to_linear_and_cubic_beziers(edited);
    local *= frame.inverse();
    auto rendered = build();
    rendered.path *= frame.inverse();
    Model updated;
    updated.frame = frame;
    struct Extension { std::size_t path, curve; double from, to; };
    std::vector<bool> used(corners.size(), false);
    for (std::size_t p = 0; p < local.size(); ++p) {
        auto n = count(local[p]);
        std::vector<std::optional<std::size_t>> removed(n), retained(n);
        std::vector<std::optional<Extension>> extensions(n);
        for (std::size_t i = 0; i < n; ++i) {
            if (!local[p].closed() && (i == 0 || i + 1 == n)) continue;
            auto previous = (i + n - 1) % n, next = (i + 1) % n;
            for (std::size_t old_p = 0; old_p < rendered.pieces.size(); ++old_p) {
                for (std::size_t j = 0; j < rendered.pieces[old_p].size(); ++j) {
                    auto c = rendered.pieces[old_p][j].corner;
                    if (!c || used[*c] || !same(local[p][i], rendered.path[old_p][j])) continue;
                    auto const &corner = corners[*c];
                    auto old_n = count(source[old_p]);
                    auto incoming_index = (corner.node + old_n - 1) % old_n;
                    auto const &incoming = source[old_p][incoming_index];
                    auto const &outgoing = source[old_p][corner.node];
                    double from = incoming.nearestTime(local[p][previous].initialPoint());
                    double in_end = incoming.nearestTime(local[p][previous].finalPoint());
                    double out_start = outgoing.nearestTime(local[p][next].initialPoint());
                    double to = outgoing.nearestTime(local[p][next].finalPoint());
                    std::unique_ptr<Geom::Curve> in_part(incoming.portion(from, in_end));
                    std::unique_ptr<Geom::Curve> out_part(outgoing.portion(out_start, to));
                    if (from >= in_end || out_start >= to || !same(*in_part, local[p][previous]) ||
                        !same(*out_part, local[p][next])) continue;
                    auto compatible = [&](std::size_t at, std::size_t curve) {
                        return !extensions[at] || (extensions[at]->path == old_p && extensions[at]->curve == curve);
                    };
                    if (!compatible(previous, incoming_index) || !compatible(next, corner.node)) continue;
                    if (extensions[previous]) extensions[previous]->to = 1;
                    else extensions[previous] = Extension{old_p, incoming_index, from, 1};
                    if (extensions[next]) extensions[next]->from = 0;
                    else extensions[next] = Extension{old_p, corner.node, 0, to};
                    removed[i] = *c;
                    retained[next] = *c;
                    used[*c] = true;
                    break;
                }
                if (removed[i]) break;
            }
        }
        Geom::Path base;
        bool started = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (removed[i]) continue;
            std::unique_ptr<Geom::Curve> curve;
            if (auto const &e = extensions[i]) curve.reset(source[e->path][e->curve].portion(e->from, e->to));
            else curve.reset(local[p][i].duplicate());
            if (!started) { base.start(curve->initialPoint()); started = true; }
            else if (!Geom::are_near(base.finalPoint(), curve->initialPoint(), 1e-5)) return false;
            if (retained[i]) {
                auto corner = corners[*retained[i]];
                corner.path = p;
                corner.node = base.size_open();
                updated.corners.push_back(corner);
            }
            base.append(*curve);
        }
        if (!started) base.start(local[p].initialPoint());
        base.close(local[p].closed());
        updated.source.push_back(std::move(base));
    }
    auto fresh = fromPath(updated.source);
    for (auto const &corner : fresh.corners) {
        if (std::none_of(updated.corners.begin(), updated.corners.end(), [&](auto const &c) {
            return c.path == corner.path && c.node == corner.node;
        })) updated.corners.push_back(corner);
    }
    if (!same(updated.render(), edited)) return false;
    *this = std::move(updated);
    return true;
}

std::string Model::write() const
{
    if (std::none_of(corners.begin(), corners.end(), [](auto const &c) {
        return c.radius > epsilon || c.style != Style::Round || c.rounding != Rounding::Absolute ||
               std::abs(c.handle_factor - 1) > epsilon;
    })) return {};
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::setprecision(17) << "1|" << sp_svg_write_path(source) << '|';
    for (auto const &c : corners) {
        stream << c.path << ',' << c.node << ',' << c.radius << ',' << int(c.style) << ',' << int(c.rounding);
        if (std::abs(c.handle_factor - 1) > epsilon) stream << ',' << c.handle_factor;
        stream << ';';
    }
    stream << '|';
    for (unsigned i = 0; i < 6; ++i) stream << (i ? "," : "") << frame[i];
    return stream.str();
}

std::optional<Model> Model::read(char const *text)
{
    if (!text || std::char_traits<char>::length(text) > 1024 * 1024) return {};
    auto parts = split(text, '|');
    if ((parts.size() != 3 && parts.size() != 4) || parts[0] != "1") return {};
    try {
        Model result = fromPath(sp_svg_read_pathv(parts[1].c_str()));
        std::vector<bool> seen(result.corners.size(), false);
        auto integer = [](std::string const &value, auto &number) {
            auto first = value.data(), last = first + value.size();
            auto parsed = std::from_chars(first, last, number);
            return parsed.ec == std::errc() && parsed.ptr == last;
        };
        if (parts.size() == 4) {
            auto values = split(parts[3], ',');
            if (values.size() != 6) return {};
            for (unsigned i = 0; i < 6; ++i) {
                std::istringstream value(values[i]);
                value.imbue(std::locale::classic());
                double number;
                value >> number;
                if (!value || !value.eof() || !std::isfinite(number)) return {};
                result.frame[i] = number;
            }
            if (std::abs(result.frame.det()) < epsilon) return {};
        }
        for (auto const &record : split(parts[2], ';')) {
            auto values = split(record, ',');
            if (values.size() != 5 && values.size() != 6) return {};
            Corner corner;
            if (!integer(values[0], corner.path) || !integer(values[1], corner.node)) return {};
            std::istringstream radius(values[2]);
            radius.imbue(std::locale::classic());
            radius >> corner.radius;
            if (!radius || !radius.eof() || !std::isfinite(corner.radius) || corner.radius < 0) return {};
            int style, rounding;
            if (!integer(values[3], style) || !integer(values[4], rounding)) return {};
            if (style < 0 || style > 2 || rounding < 0 || rounding > 1) return {};
            corner.style = Style(style);
            corner.rounding = Rounding(rounding);
            if (values.size() == 6) {
                std::istringstream factor(values[5]);
                factor.imbue(std::locale::classic());
                factor >> corner.handle_factor;
                if (!factor || !factor.eof() || !std::isfinite(corner.handle_factor) ||
                    corner.handle_factor < .5 || corner.handle_factor > 1.5) return {};
            }
            auto match = std::find_if(result.corners.begin(), result.corners.end(), [&](auto const &c) {
                return c.path == corner.path && c.node == corner.node;
            });
            if (match == result.corners.end()) return {};
            auto index = std::distance(result.corners.begin(), match);
            if (seen[index]) return {};
            seen[index] = true;
            *match = corner;
        }
        return result;
    } catch (std::exception const &) { return {}; }
}

} // namespace Inkscape::UI::LiveCorners
