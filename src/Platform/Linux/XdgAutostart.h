#pragma once

// XDG autostart entries (#2541): the .desktop files a desktop session starts at sign-in, from
// $XDG_CONFIG_HOME/autostart (~/.config/autostart) and each $XDG_CONFIG_DIRS/autostart (/etc/xdg/autostart),
// as the Desktop Application Autostart Specification reads them. A file in an earlier directory hides a
// file of the same name in a later one, which is how a user turns a system entry off without touching it:
// a copy in ~/.config/autostart with Hidden=true. Pure parsing and rewriting here; LinuxStartupProbe and
// LinuxStartupActions do the file system.

#include "Platform/AsciiTrim.h"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Platform::XdgAutostart
{

/// What an autostart file's [Desktop Entry] group says (unlocalised keys only).
struct Entry
{
    std::string name;    ///< Name
    std::string exec;    ///< Exec: the command line, field codes (%f, %u, ...) and all
    std::string tryExec; ///< TryExec: the program whose absence means the entry is skipped
    std::string icon;    ///< Icon
    bool hidden = false; ///< Hidden=true: the entry is off (the spec's "deleted")
    /// X-GNOME-Autostart-enabled, when given: GNOME's and others' own off switch.
    std::optional<bool> gnomeEnabled;
    std::vector<std::string> onlyShowIn; ///< OnlyShowIn: the desktops it starts in, when given
    std::vector<std::string> notShowIn;  ///< NotShowIn: the desktops it doesn't start in
    bool application = true;             ///< Type=Application (or no Type)

    /// Whether the entry is switched on: not Hidden, and not X-GNOME-Autostart-enabled=false.
    [[nodiscard]] bool enabled() const noexcept
    {
        return !hidden && gnomeEnabled.value_or(true);
    }
};

/// The ';'-separated words of a desktop file's list value ("GNOME;KDE;"), empty ones dropped.
[[nodiscard]] inline std::vector<std::string> splitList(std::string_view value, char separator = ';')
{
    std::vector<std::string> words;
    while (!value.empty())
    {
        const std::size_t end = value.find(separator);
        const std::string_view word = trimAscii(value.substr(0, end));
        if (!word.empty())
        {
            words.emplace_back(word);
        }
        value.remove_prefix(end == std::string_view::npos ? value.size() : end + 1);
    }
    return words;
}

/// The [Desktop Entry] group of an autostart file's @p text; nullopt when it has none.
[[nodiscard]] inline std::optional<Entry> parse(std::string_view text)
{
    Entry entry;
    bool inMainGroup = false;
    bool sawMainGroup = false;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view line = trimAscii(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.empty() || line.front() == '#')
        {
            continue;
        }
        if (line.front() == '[')
        {
            inMainGroup = (line == "[Desktop Entry]");
            sawMainGroup = sawMainGroup || inMainGroup;
            continue;
        }
        const std::size_t equals = line.find('=');
        if (!inMainGroup || equals == std::string_view::npos)
        {
            continue;
        }
        const std::string_view key = trimAscii(line.substr(0, equals));
        const std::string_view value = trimAscii(line.substr(equals + 1));
        if (key == "Name")
        {
            entry.name = value;
        }
        else if (key == "Exec")
        {
            entry.exec = value;
        }
        else if (key == "TryExec")
        {
            entry.tryExec = value;
        }
        else if (key == "Icon")
        {
            entry.icon = value;
        }
        else if (key == "Hidden")
        {
            entry.hidden = (value == "true");
        }
        else if (key == "X-GNOME-Autostart-enabled")
        {
            entry.gnomeEnabled = (value != "false");
        }
        else if (key == "OnlyShowIn")
        {
            entry.onlyShowIn = splitList(value);
        }
        else if (key == "NotShowIn")
        {
            entry.notShowIn = splitList(value);
        }
        else if (key == "Type")
        {
            entry.application = (value == "Application");
        }
    }
    if (!sawMainGroup)
    {
        return std::nullopt;
    }
    return entry;
}

/// Whether @p entry starts in the session whose $XDG_CURRENT_DESKTOP is @p currentDesktops (a
/// ':'-separated list, "ubuntu:GNOME"): one of them is in OnlyShowIn, when given, and none is in
/// NotShowIn. With no desktop named, an OnlyShowIn entry doesn't start.
[[nodiscard]] inline bool startsIn(const Entry& entry, std::string_view currentDesktops)
{
    const std::vector<std::string> desktops = splitList(currentDesktops, ':');
    const auto named = [&desktops](const std::vector<std::string>& list)
    {
        for (const std::string& desktop : desktops)
        {
            for (const std::string& listed : list)
            {
                if (listed == desktop)
                {
                    return true;
                }
            }
        }
        return false;
    };
    if (!entry.onlyShowIn.empty() && !named(entry.onlyShowIn))
    {
        return false;
    }
    return !named(entry.notShowIn);
}

/// Where autostart files are looked for, the user's first: $XDG_CONFIG_HOME/autostart (else
/// ~/.config/autostart), then each $XDG_CONFIG_DIRS directory's (else /etc/xdg/autostart). Relative
/// directories are ignored, as the Base Directory Specification says.
struct Dirs
{
    std::filesystem::path user; ///< Empty when neither XDG_CONFIG_HOME nor HOME is usable.
    std::vector<std::filesystem::path> system;
};

[[nodiscard]] inline Dirs dirs(std::string_view home, std::string_view xdgConfigHome, std::string_view xdgConfigDirs)
{
    Dirs found;
    if (!xdgConfigHome.empty() && xdgConfigHome.front() == '/')
    {
        found.user = std::filesystem::path(xdgConfigHome) / "autostart";
    }
    else if (!home.empty() && home.front() == '/')
    {
        found.user = std::filesystem::path(home) / ".config" / "autostart";
    }
    for (const std::string& dir : splitList(xdgConfigDirs.empty() ? std::string_view("/etc/xdg") : xdgConfigDirs, ':'))
    {
        if (dir.front() == '/')
        {
            found.system.push_back(std::filesystem::path(dir) / "autostart");
        }
    }
    return found;
}

/// @p text with its [Desktop Entry] group switched on or off: Hidden set to @p enabled's opposite
/// (the line added right after the group header when there was none), and X-GNOME-Autostart-enabled,
/// when present, set to match. Every other line is kept as it was. nullopt when @p text has no
/// [Desktop Entry] group.
[[nodiscard]] inline std::optional<std::string> withEnabled(std::string_view text, bool enabled)
{
    const std::string hiddenLine = enabled ? "Hidden=false" : "Hidden=true";
    const std::string gnomeLine = enabled ? "X-GNOME-Autostart-enabled=true" : "X-GNOME-Autostart-enabled=false";
    std::string out;
    out.reserve(text.size() + hiddenLine.size() + 1);
    bool inMainGroup = false;
    bool sawMainGroup = false;
    bool wroteHidden = false;
    while (!text.empty())
    {
        const std::size_t newline = text.find('\n');
        const std::string_view rawLine = text.substr(0, newline);
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        const std::string_view line = trimAscii(rawLine);
        if (!line.empty() && line.front() == '[')
        {
            inMainGroup = (line == "[Desktop Entry]");
            out.append(rawLine).push_back('\n');
            if (inMainGroup && !sawMainGroup)
            {
                sawMainGroup = true;
                out.append(hiddenLine).push_back('\n');
                wroteHidden = true;
            }
            continue;
        }
        if (inMainGroup)
        {
            const std::size_t equals = line.find('=');
            const std::string_view key = equals == std::string_view::npos ? std::string_view{} : trimAscii(line.substr(0, equals));
            if (key == "Hidden")
            {
                continue; // written after the header
            }
            if (key == "X-GNOME-Autostart-enabled")
            {
                out.append(gnomeLine).push_back('\n');
                continue;
            }
        }
        out.append(rawLine).push_back('\n');
    }
    if (!wroteHidden)
    {
        return std::nullopt;
    }
    return out;
}

} // namespace Platform::XdgAutostart
