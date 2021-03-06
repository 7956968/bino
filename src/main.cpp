/*
 * This file is part of bino, a 3D video player.
 *
 * Copyright (C) 2010-2011
 * Martin Lambers <marlam@marlam.de>
 * Stefan Eilemann <eile@eyescale.ch>
 * Frédéric Devernay <Frederic.Devernay@inrialpes.fr>
 * Joe <cuchac@email.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <limits>
#include <locale.h>

#if (defined _WIN32 || defined __WIN32__) && !defined __CYGWIN__
# include <windows.h>
#endif

#ifdef __APPLE__
# include <mach-o/dyld.h> // for _NSGetExecutablePath()
#endif

#include <QCoreApplication>
#include <QApplication>
#include <QtGlobal>
#include <QTextCodec>

#include "gettext.h"
#define _(string) gettext(string)

#include "dbg.h"
#include "msg.h"
#include "opt.h"

#include "player.h"
#include "player_qt.h"
#if HAVE_LIBEQUALIZER
# include "player_equalizer.h"
#endif
#if HAVE_LIBLIRCCLIENT
# include "lirc.h"
#endif
#include "lib_versions.h"


/* Return the directory containing our locale data (translated messages). */
static const char *localedir()
{
#if (defined _WIN32 || defined __WIN32__) && !defined __CYGWIN__
    /* Windows: D/../locale, where D contains the program binary. If something
     * goes wrong, we fall back to "..\\locale", which at least works if the
     * current directory contains the program binary. */
    static const char *rel_locale = "..\\locale";
    static char buffer[MAX_PATH + 10];  // leave space to append rel_locale
    DWORD v = GetModuleFileName(NULL, buffer, MAX_PATH);
    if (v == 0 || v >= MAX_PATH)
    {
        return rel_locale;
    }
    char *backslash = strrchr(buffer, '\\');
    if (!backslash)
    {
        return rel_locale;
    }
    strcpy(backslash + 1, "..\\locale");
    return buffer;
#else
# ifdef __APPLE__
    try
    {
        static char buffer[PATH_MAX];
        uint32_t buffersize = sizeof(buffer);
        int32_t check = _NSGetExecutablePath(buffer, &buffersize);
        if (check != 0)
        {
            throw 0;
        }
        char *pch = std::strrchr(buffer, '/');
        if (pch == NULL)
        {
            throw 0;
        }
        *pch = 0;
        pch = std::strrchr(buffer, '/');
        if (pch == NULL)
        {
            throw 0;
        }
        // check that I'm an application
        if (std::strncmp(pch, "/MacOS", 6) != 0)
        {
            throw 0;
        }
        const char *subdir = "/Resources/locale";
        size_t subdirlen = std::strlen(subdir) + 1;
        // check that there's enough room left
        if (sizeof(buffer) - (buffer-pch) <= subdirlen)
        {
            throw 0;
        }
        std::strncpy(pch, subdir, subdirlen);
        return buffer;
    }
    catch (...)
    {
    }
# endif
    /* GNU/Linux and others: fixed directory defined by LOCALEDIR */
    return LOCALEDIR;
#endif
}

static void qt_msg_handler(QtMsgType type, const char *msg)
{
    switch (type)
    {
    case QtDebugMsg:
        msg::dbg("%s", msg);
        break;
    case QtWarningMsg:
        msg::wrn("%s", msg);
        break;
    case QtCriticalMsg:
        msg::err("%s", msg);
        break;
    case QtFatalMsg:
        msg::err("%s", msg);
        std::exit(1);
    }
}

// Handle a log file that may be set via the --log-file option; see below
static FILE *logf = NULL;
static void close_log_file(void)
{
    if (logf)
    {
        (void)std::fclose(logf);
    }
}


int main(int argc, char *argv[])
{
    /* Initialization: gettext */
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, localedir());
    textdomain(PACKAGE);

    /* Initialization: messages */
    char *program_name = strrchr(argv[0], '/');
    program_name = program_name ? program_name + 1 : argv[0];
    msg::set_level(msg::INF);
    msg::set_program_name(program_name);
    msg::set_columns_from_env();
    dbg::init_crashhandler();

    /* Initialization: Qt */
#ifdef Q_WS_X11
    const char *display = getenv("DISPLAY");
    bool have_display = (display && display[0] != '\0');
#else
    bool have_display = true;
#endif
    qInstallMsgHandler(qt_msg_handler);
    QApplication *qt_app = new QApplication(argc, argv, have_display);
    QTextCodec::setCodecForCStrings(QTextCodec::codecForLocale()); // necessary for i18n via gettext
    QCoreApplication::setOrganizationName("Bino");
    QCoreApplication::setOrganizationDomain("bino3d.org");
    QCoreApplication::setApplicationName(PACKAGE_NAME);

    /* Command line handling */
    std::vector<opt::option *> options;
    opt::info help("help", '\0', opt::optional);
    options.push_back(&help);
    opt::info version("version", '\0', opt::optional);
    options.push_back(&version);
    opt::flag no_gui("no-gui", 'n', opt::optional);
    options.push_back(&no_gui);
    opt::val<std::string> log_file("log-file", '\0', opt::optional);
    options.push_back(&log_file);
    std::vector<std::string> log_levels;
    log_levels.push_back("debug");
    log_levels.push_back("info");
    log_levels.push_back("warning");
    log_levels.push_back("error");
    log_levels.push_back("quiet");
    opt::val<std::string> log_level("log-level", 'L', opt::optional, log_levels, "");
    options.push_back(&log_level);
    std::vector<std::string> device_types;
    device_types.push_back("default");
    device_types.push_back("firewire");
    device_types.push_back("x11");
    opt::val<std::string> device_type("device-type", '\0', opt::optional, device_types, "");
    options.push_back(&device_type);
    opt::tuple<int> device_frame_size("device-frame-size", '\0', opt::optional,
            1, std::numeric_limits<int>::max(), std::vector<int>(2, 0), 2, "x");
    options.push_back(&device_frame_size);
    opt::tuple<int> device_frame_rate("device-frame-rate", '\0', opt::optional,
            1, std::numeric_limits<int>::max(), std::vector<int>(2, 0), 2, "/");
    options.push_back(&device_frame_rate);
    opt::val<std::string> lirc_config("lirc-config", '\0', opt::optional);
    options.push_back(&lirc_config);
    std::vector<std::string> input_modes;
    input_modes.push_back("mono");
    input_modes.push_back("separate-left-right");
    input_modes.push_back("separate-right-left");
    input_modes.push_back("top-bottom");
    input_modes.push_back("top-bottom-half");
    input_modes.push_back("bottom-top");
    input_modes.push_back("bottom-top-half");
    input_modes.push_back("left-right");
    input_modes.push_back("left-right-half");
    input_modes.push_back("right-left");
    input_modes.push_back("right-left-half");
    input_modes.push_back("even-odd-rows");
    input_modes.push_back("odd-even-rows");
    opt::val<std::string> input_mode("input", 'i', opt::optional, input_modes, "");
    options.push_back(&input_mode);
    opt::val<int> video("video", 'v', opt::optional, 1, 999, 1);
    options.push_back(&video);
    opt::val<int> audio("audio", 'a', opt::optional, 1, 999, 1);
    options.push_back(&audio);
    opt::val<int> subtitle("subtitle", 's', opt::optional, 0, 999, 0);
    options.push_back(&subtitle);
    std::vector<std::string> video_output_modes;
    video_output_modes.push_back("mono-left");
    video_output_modes.push_back("mono-right");
    video_output_modes.push_back("top-bottom");
    video_output_modes.push_back("top-bottom-half");
    video_output_modes.push_back("left-right");
    video_output_modes.push_back("left-right-half");
    video_output_modes.push_back("even-odd-rows");
    video_output_modes.push_back("even-odd-columns");
    video_output_modes.push_back("checkerboard");
    video_output_modes.push_back("hdmi-frame-pack");
    video_output_modes.push_back("red-cyan-monochrome");
    video_output_modes.push_back("red-cyan-half-color");
    video_output_modes.push_back("red-cyan-full-color");
    video_output_modes.push_back("red-cyan-dubois");
    video_output_modes.push_back("green-magenta-monochrome");
    video_output_modes.push_back("green-magenta-half-color");
    video_output_modes.push_back("green-magenta-full-color");
    video_output_modes.push_back("green-magenta-dubois");
    video_output_modes.push_back("amber-blue-monochrome");
    video_output_modes.push_back("amber-blue-half-color");
    video_output_modes.push_back("amber-blue-full-color");
    video_output_modes.push_back("amber-blue-dubois");
    video_output_modes.push_back("red-green-monochrome");
    video_output_modes.push_back("red-blue-monochrome");
    video_output_modes.push_back("stereo");
    video_output_modes.push_back("equalizer");
    video_output_modes.push_back("equalizer-3d");
    opt::val<std::string> video_output_mode("output", 'o', opt::optional, video_output_modes, "");
    options.push_back(&video_output_mode);
    opt::flag swap_eyes("swap-eyes", 'S', opt::optional);
    options.push_back(&swap_eyes);
    opt::flag fullscreen("fullscreen", 'f', opt::optional);
    options.push_back(&fullscreen);
    opt::tuple<int> fullscreen_screens("fullscreen-screens", '\0', opt::optional, 1, 16);
    options.push_back(&fullscreen_screens);
    opt::flag fullscreen_flip_left("fullscreen-flip-left", '\0', opt::optional);
    options.push_back(&fullscreen_flip_left);
    opt::flag fullscreen_flop_left("fullscreen-flop-left", '\0', opt::optional);
    options.push_back(&fullscreen_flop_left);
    opt::flag fullscreen_flip_right("fullscreen-flip-right", '\0', opt::optional);
    options.push_back(&fullscreen_flip_right);
    opt::flag fullscreen_flop_right("fullscreen-flop-right", '\0', opt::optional);
    options.push_back(&fullscreen_flop_right);
    opt::val<float> zoom("zoom", 'z', opt::optional, 0.0f, 1.0f, parameters().zoom);
    options.push_back(&zoom);
    opt::flag center("center", 'c', opt::optional);
    options.push_back(&center);
    opt::val<std::string> subtitle_encoding("subtitle-encoding", '\0', opt::optional, parameters().subtitle_encoding);
    options.push_back(&subtitle_encoding);
    opt::val<std::string> subtitle_font("subtitle-font", '\0', opt::optional, parameters().subtitle_font);
    options.push_back(&subtitle_font);
    opt::val<int> subtitle_size("subtitle-size", '\0', opt::optional, 1, 999, parameters().subtitle_size);
    options.push_back(&subtitle_size);
    opt::val<float> subtitle_scale("subtitle-scale", '\0', opt::optional, 0.0f, false,
            std::numeric_limits<float>::max(), true, parameters().subtitle_scale);
    options.push_back(&subtitle_scale);
    opt::color subtitle_color("subtitle-color", '\0', opt::optional);
    options.push_back(&subtitle_color);
    opt::val<float> subtitle_parallax("subtitle-parallax", '\0', opt::optional, -1.0f, +1.0f, parameters().subtitle_parallax);
    options.push_back(&subtitle_parallax);
    opt::val<float> parallax("parallax", 'P', opt::optional, -1.0f, +1.0f, parameters().parallax);
    options.push_back(&parallax);
    opt::tuple<float> crosstalk("crosstalk", 'C', opt::optional, 0.0f, 1.0f, std::vector<float>(3, parameters().crosstalk_r), 3);
    options.push_back(&crosstalk);
    opt::val<float> ghostbust("ghostbust", 'G', opt::optional, 0.0f, 1.0f, parameters().ghostbust);
    options.push_back(&ghostbust);
    opt::flag benchmark("benchmark", 'b', opt::optional);
    options.push_back(&benchmark);
    opt::flag loop("loop", 'l', opt::optional);
    options.push_back(&loop);
    // Accept some Equalizer options. These are passed to Equalizer for interpretation.
    opt::val<std::string> eq_server("eq-server", '\0', opt::optional);
    options.push_back(&eq_server);
    opt::val<std::string> eq_config("eq-config", '\0', opt::optional);
    options.push_back(&eq_config);
    opt::val<std::string> eq_listen("eq-listen", '\0', opt::optional);
    options.push_back(&eq_listen);
    opt::val<std::string> eq_logfile("eq-logfile", '\0', opt::optional);
    options.push_back(&eq_logfile);
    opt::val<std::string> eq_render_client("eq-render-client", '\0', opt::optional);
    options.push_back(&eq_render_client);

    std::vector<std::string> arguments;
#ifdef __APPLE__
    // strip -psn* option when launching from the Finder on mac
    if (argc > 1 && strncmp("-psn", argv[1], 4) == 0)
    {
        argc--;
        argv++;
    }
#endif
    if (!opt::parse(argc, argv, options, 0, -1, arguments))
    {
        return 1;
    }
    if (!log_file.value().empty())
    {
        logf = std::fopen(log_file.value().c_str(), "a");
        if (!logf)
        {
            msg::err(_("%s: %s"), log_file.value().c_str(), std::strerror(errno));
            return 1;
        }
        std::atexit(close_log_file);
        msg::set_file(logf);
        msg::set_columns(80);
    }

    if (version.value())
    {
        msg::req(_("%s version %s"), PACKAGE_NAME, VERSION);
        msg::req(4, _("Copyright (C) 2011 the Bino developers."));
        msg::req_txt(4, _("This is free software. You may redistribute copies of it "
                    "under the terms of the GNU General Public License. "
                    "There is NO WARRANTY, to the extent permitted by law."));
        msg::req(_("Platform:"));
        msg::req(4, "%s", PLATFORM);
        msg::req(_("Libraries used:"));
        std::vector<std::string> v = lib_versions(false);
        for (size_t i = 0; i < v.size(); i++)
        {
            msg::req(4, "%s", v[i].c_str());
        }
    }
    if (help.value())
    {
        /* TRANSLATORS: This is the --help text. Translate only the description,
           not the option names. Please keep a proper indentation with spaces, and
           keep the line length limited. */
        msg::req_txt(_("Usage:\n"
                    "  %s [option...] [file...]\n"
                    "\n"
                    "Options:\n"
                    "  --help                   Print help.\n"
                    "  --version                Print version.\n"
                    "  -n|--no-gui              Do not use the GUI, just show a plain window.\n"
                    "  --log-file=FILE          Append all log messages to the given file.\n"
                    "  -L|--log-level=LEVEL     Set log level (debug/info/warning/error/quiet).\n"
                    "  --device-type=TYPE       Type of input device: default, firewire, x11.\n"
                    "  --device-frame-size=WxH  Request frame size WxH from input device.\n"
                    "  --device-frame-rate=N/D  Request frame rate N/D from input device.\n"
                    "  --lirc-config=FILE       Use the given LIRC configuration file. This\n"
                    "                           option can be used more than once.\n"
                    "  -v|--video=STREAM        Select video stream (1-n, depending on input).\n"
                    "  -a|--audio=STREAM        Select audio stream (1-n, depending on input).\n"
                    "  -s|--subtitle=STREAM     Select subtitle stream (0-n, dep. on input).\n"
                    "  -i|--input=TYPE          Select input type (default autodetect):\n"
                    "    mono                     Single view.\n"
                    "    separate-left-right      Left/right separate streams, left first.\n"
                    "    separate-right-left      Left/right separate streams, right first.\n"
                    "    top-bottom               Left top, right bottom.\n"
                    "    top-bottom-half          Left top, right bottom, half height.\n"
                    "    bottom-top               Left bottom, right top.\n"
                    "    bottom-top-half          Left bottom, right top, half height.\n"
                    "    left-right               Left left, right right.\n"
                    "    left-right-half          Left left, right right, half width.\n"
                    "    right-left               Left right, right left.\n"
                    "    right-left-half          Left right, right left, half width.\n"
                    "    even-odd-rows            Left even rows, right odd rows.\n"
                    "    odd-even-rows            Left odd rows, right even rows.\n"
                    "  -o|--output=TYPE         Select output type:\n"
                    "    mono-left                Only left.\n"
                    "    mono-right               Only right.\n"
                    "    top-bottom               Left top, right bottom.\n"
                    "    top-bottom-half          Left top, right bottom, half height.\n"
                    "    left-right               Left left, right right.\n"
                    "    left-right-half          Left left, right right, half width.\n"
                    "    even-odd-rows            Left even rows, right odd rows.\n"
                    "    even-odd-columns         Left even columns, right odd columns.\n"
                    "    checkerboard             Left and right in checkerboard pattern.\n"
                    "    hdmi-frame-pack          HDMI frame packing mode.\n"
                    "    red-cyan-monochrome      Red/cyan anaglyph, monochrome method.\n"
                    "    red-cyan-half-color      Red/cyan anaglyph, half color method.\n"
                    "    red-cyan-full-color      Red/cyan anaglyph, full color method.\n"
                    "    red-cyan-dubois          Red/cyan anaglyph, Dubois method.\n"
                    "    green-magenta-monochrome Green/magenta anaglyph, monochrome method.\n"
                    "    green-magenta-half-color Green/magenta anaglyph, half color method.\n"
                    "    green-magenta-full-color Green/magenta anaglyph, full color method.\n"
                    "    green-magenta-dubois     Green/magenta anaglyph, Dubois method.\n"
                    "    amber-blue-monochrome    Amber/blue anaglyph, monochrome method.\n"
                    "    amber-blue-half-color    Amber/blue anaglyph, half color method.\n"
                    "    amber-blue-full-color    Amber/blue anaglyph, full color method.\n"
                    "    amber-blue-dubois        Amber/blue anaglyph, Dubois method.\n"
                    "    red-green-monochrome     Red/green anaglyph, monochrome method.\n"
                    "    red-blue-monochrome      Red/blue anaglyph, monochrome method.\n"
                    "    stereo                   OpenGL quad-buffered stereo.\n"
                    "    equalizer                Multi-display via Equalizer (2D setup).\n"
                    "    equalizer-3d             Multi-display via Equalizer (3D setup).\n"
                    "  -S|--swap-eyes           Swap left/right view.\n"
                    "  -f|--fullscreen          Fullscreen.\n"
                    "  --fullscreen-screens=    Use the listed screens in fullscreen mode.\n"
                    "     [S0[,S1[,...]]]       Screen numbers start with 1. The default\n"
                    "                           (empty list) is to use the primary screen.\n"
                    "  --fullscreen-flip-left   Flip left view vertically when fullscreen.\n"
                    "  --fullscreen-flop-left   Flop left view horizontally when fullscreen.\n"
                    "  --fullscreen-flip-right  Flip right view vertically when fullscreen.\n"
                    "  --fullscreen-flop-right  Flop right view horizontally when fullscreen.\n"
                    "  -z|--zoom=Z              Set zoom for wide videos (0=off to 1=full).\n"
                    "  -c|--center              Center window on screen.\n"
                    "  --subtitle-encoding=ENC  Set subtitle encoding.\n"
                    "  --subtitle-font=FONT     Set subtitle font name.\n"
                    "  --subtitle-size=N        Set subtitle font size.\n"
                    "  --subtitle-scale=S       Set subtitle scale factor.\n"
                    "  --subtitle-color=COLOR   Set subtitle color, in [AA]RRGGBB format.\n"
                    "  --subtitle-parallax=VAL  Subtitle parallax adjustment (-1 to +1).\n"
                    "  -P|--parallax=VAL        Parallax adjustment (-1 to +1).\n"
                    "  -C|--crosstalk=VAL       Crosstalk leak level (0 to 1); comma-separated\n"
                    "                           values for the R,G,B channels.\n"
                    "  -G|--ghostbust=VAL       Amount of ghostbusting to apply (0 to 1).\n"
                    "  -b|--benchmark           Benchmark mode (no audio, show fps).\n"
                    "  -l|--loop                Loop the input media.\n"
                    "\n"
                    "Interactive control:\n"
                    "  ESC                      Leave fullscreen mode, or quit.\n"
                    "  q                        Quit.\n"
                    "  p or SPACE               Pause / unpause.\n"
                    "  f                        Toggle fullscreen.\n"
                    "  c                        Center window.\n"
                    "  e                        Swap left/right eye.\n"
                    "  v                        Cycle through available video streams.\n"
                    "  a                        Cycle through available audio streams.\n"
                    "  s                        Cycle through available subtitle streams.\n"
                    "  1, 2                     Adjust contrast.\n"
                    "  3, 4                     Adjust brightness.\n"
                    "  5, 6                     Adjust hue.\n"
                    "  7, 8                     Adjust saturation.\n"
                    "  [, ]                     Adjust parallax.\n"
                    "  (, )                     Adjust ghostbusting.\n"
                    "  <, >                     Adjust zoom for wide videos.\n"
                    "  left, right              Seek 10 seconds backward / forward.\n"
                    "  up, down                 Seek 1 minute backward / forward.\n"
                    "  page up, page down       Seek 10 minutes backward / forward.\n"
                    "  Mouse click              Seek according to horizontal click position."),
                program_name);
    }
    if (version.value() || help.value())
    {
        return 0;
    }

#if HAVE_LIBEQUALIZER
    if (arguments.size() > 0 && arguments[0] == "--eq-client")
    {
        try
        {
            player_equalizer *player = new player_equalizer(&argc, argv, true);
            delete player;
        }
        catch (std::exception &e)
        {
            msg::err("%s", e.what());
            return 1;
        }
        return 0;
    }
#endif

    bool equalizer = false;
    bool equalizer_flat_screen = true;
    player_init_data init_data;
    init_data.log_level = msg::level();
    if (log_level.value() == "")
    {
        init_data.log_level = msg::INF;
    }
    else if (log_level.value() == "debug")
    {
        init_data.log_level = msg::DBG;
    }
    else if (log_level.value() == "info")
    {
        init_data.log_level = msg::INF;
    }
    else if (log_level.value() == "warning")
    {
        init_data.log_level = msg::WRN;
    }
    else if (log_level.value() == "error")
    {
        init_data.log_level = msg::ERR;
    }
    else if (log_level.value() == "quiet")
    {
        init_data.log_level = msg::REQ;
    }
    if (device_type.value() == "")
    {
        init_data.dev_request.device =
            (arguments.size() == 1 && arguments[0].substr(0, 5) == "/dev/"
             ? device_request::sys_default : device_request::no_device);
    }
    else
    {
        init_data.dev_request.device =
            (device_type.value() == "firewire" ? device_request::firewire
             : device_type.value() == "x11" ? device_request::x11
             : device_request::sys_default);
    }
    init_data.dev_request.width = device_frame_size.value()[0];
    init_data.dev_request.height = device_frame_size.value()[1];
    init_data.dev_request.frame_rate_num = device_frame_rate.value()[0];
    init_data.dev_request.frame_rate_den = device_frame_rate.value()[1];
    init_data.urls = arguments;
    init_data.video_stream = video.value() - 1;
    init_data.audio_stream = audio.value() - 1;
    init_data.subtitle_stream = subtitle.value() - 1;
    if (input_mode.value() == "")
    {
        init_data.stereo_layout_override = false;
    }
    else
    {
        init_data.stereo_layout_override = true;
        video_frame::stereo_layout_from_string(input_mode.value(), init_data.stereo_layout, init_data.stereo_layout_swap);
    }
    if (video_output_mode.value() == "")
    {
        init_data.stereo_mode_override = false;
    }
    else if (video_output_mode.value() == "equalizer")
    {
        equalizer = true;
        equalizer_flat_screen = true;
        init_data.stereo_mode_override = true;
        init_data.stereo_mode = parameters::mono_left;
        init_data.stereo_mode_swap = false;
    }
    else if (video_output_mode.value() == "equalizer-3d")
    {
        equalizer = true;
        equalizer_flat_screen = false;
        init_data.stereo_mode_override = true;
        init_data.stereo_mode = parameters::mono_left;
        init_data.stereo_mode_swap = false;
    }
    else
    {
        init_data.stereo_mode_override = true;
        parameters::stereo_mode_from_string(video_output_mode.value(), init_data.stereo_mode, init_data.stereo_mode_swap);
        init_data.stereo_mode_swap = swap_eyes.value();
    }
    init_data.params.stereo_mode_swap = swap_eyes.value();
    init_data.fullscreen = fullscreen.value();
    if (fullscreen_screens.values().size() > 0)
    {
        init_data.params.fullscreen_screens = 0;
        for (size_t i = 0; i < fullscreen_screens.value().size(); i++)
        {
            init_data.params.fullscreen_screens |= (1 << (fullscreen_screens.value()[i] - 1));
        }
    }
    if (fullscreen_flip_left.values().size() > 0)
    {
        init_data.params.fullscreen_flip_left = fullscreen_flip_left.value();
    }
    if (fullscreen_flop_left.values().size() > 0)
    {
        init_data.params.fullscreen_flop_left = fullscreen_flop_left.value();
    }
    if (fullscreen_flip_right.values().size() > 0)
    {
        init_data.params.fullscreen_flip_right = fullscreen_flip_right.value();
    }
    if (fullscreen_flop_right.values().size() > 0)
    {
        init_data.params.fullscreen_flop_right = fullscreen_flop_right.value();
    }
    init_data.params.zoom = zoom.value();
    init_data.center = center.value();
    init_data.params.subtitle_encoding = subtitle_encoding.value();
    init_data.params.subtitle_font = subtitle_font.value();
    init_data.params.subtitle_size = subtitle_size.value();
    init_data.params.subtitle_scale = subtitle_scale.value();
    if (!subtitle_color.values().empty())
    {
        init_data.params.subtitle_color = subtitle_color.value();
    }
    init_data.params.subtitle_parallax = subtitle_parallax.value();
    init_data.params.parallax = parallax.value();
    init_data.params.crosstalk_r = crosstalk.value()[0];
    init_data.params.crosstalk_g = crosstalk.value()[1];
    init_data.params.crosstalk_b = crosstalk.value()[2];
    init_data.params.ghostbust = ghostbust.value();
    init_data.benchmark = benchmark.value();
    if (init_data.benchmark)
    {
        msg::inf(_("Benchmark mode: audio and time synchronization disabled."));
    }
    init_data.params.loop_mode = (loop.value() ? parameters::loop_current : parameters::no_loop);

#if HAVE_LIBLIRCCLIENT
    lircclient lirc(PACKAGE, lirc_config.values());
    try
    {
        lirc.init();
    }
    catch (std::exception &e)
    {
        msg::wrn("%s", e.what());
    }
#else
    if (lirc_config.values().size() > 0)
    {
        msg::wrn(_("This version of Bino was compiled without support for LIRC."));
    }
#endif

    int retval = 0;
    player *player = NULL;
    try
    {
        if (equalizer)
        {
#if HAVE_LIBEQUALIZER
            player = new class player_equalizer(&argc, argv, equalizer_flat_screen);
#else
            throw exc(_("This version of Bino was compiled without support for Equalizer."));
#endif
        }
        else
        {
            if (!have_display)
            {
                throw exc(_("Cannot connect to X server."));
            }
            else if (!no_gui.value())
            {
                if (log_level.value() == "")
                {
                    init_data.log_level = msg::WRN;         // Be silent by default when the GUI is used
                }
                init_data.fullscreen = false;               // GUI overrides fullscreen setting
                init_data.center = false;                   // GUI overrides center flag
                player = new class player_qt();
            }
            else
            {
                if (arguments.size() == 0)
                {
                    throw exc(_("No video to play."));
                }
                player = new class player();
            }
        }
        player->open(init_data);
        player->run();
    }
    catch (std::exception &e)
    {
        msg::err("%s", e.what());
        retval = 1;
    }
    if (player)
    {
        try { player->close(); } catch (...) {}
        delete player;
    }

#if HAVE_LIBLIRCCLIENT
    lirc.deinit();
#endif

    delete qt_app;

    return retval;
}
