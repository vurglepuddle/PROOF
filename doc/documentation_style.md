[Inkscape Developer Documentation](readme.md) /

# Styleguide for developer documentation

This page explains how documentation is stored that is only relevant to Inkscape developers, not to normal users.

Inkscape developer documentation is only available in English. There are no translations because the source comments and issue tracker are also only in English.

## Code-specific documentation

Documentation related to specific code files is done within the file using Doxygen.

## General documentation

There is also developer documentation that *does not* directly belong to specific code files. For example, we have code style guidelines and instructions how to build Inkscape.

Such documentation is stored in Markdown files in `doc/`. Currently, we use a very simple approach with Markdown and "handcrafted" navigation links: Every directory therein contains an index file `readme.md` that introduces the topic, links to other `.md` files on the same level and to the `readme.md` files of the subdirectories. Every file has a header linking to the above `readme.md` files; see the existing files for an example.

Exceptions:
- README.md and INSTALL.md are stored in the main directory to match the standard conventions.
- Documentation belonging to a specific code *directory* can be stored therein with the filename `README.md`.

## Other locations

Short-lived information, e.g., issues, roadmap and proposals are stored in the [GitLab issue trackers](https://gitlab.com/inkscape/) where appropriate and else in the [Wiki](https://wiki.inkscape.org/).
