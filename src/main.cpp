// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#ifdef USE_KDSINGLEAPPLICATION
#include "kdsingleapplication.h"
#ifdef Q_OS_UNIX
#include "core/signaldaemon.h"
#include "csignal"
#endif
#endif

#include "abstractlogger.h"
#include "src/cli/commandlineparser.h"
#include "src/config/cacheutils.h"
#include "src/config/styleoverride.h"
#include "src/core/capturerequest.h"
#include "src/core/flameshot.h"
#include "src/core/flameshotdaemon.h"
#include "src/utils/confighandler.h"
#include "src/utils/filenamehandler.h"
#include "src/utils/pathinfo.h"
#include "src/utils/valuehandler.h"
#include <QApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QSharedMemory>
#include <QTimer>
#include <QTranslator>
#if defined(Q_OS_LINUX) || defined(Q_OS_UNIX)
#include "abstractlogger.h"
#include "src/core/flameshotdbusadapter.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <desktopinfo.h>
#endif

// Required for saving button list QList<CaptureTool::Type>
Q_DECLARE_METATYPE(QList<int>)

#if defined(USE_KDSINGLEAPPLICATION) && defined(Q_OS_UNIX)
static int setup_unix_signal_handlers()
{
    struct sigaction sint, term;

    sint.sa_handler = SignalDaemon::intSignalHandler;
    sigemptyset(&sint.sa_mask);
    sint.sa_flags = 0;
    sint.sa_flags |= SA_RESTART;

    if (sigaction(SIGINT, &sint, 0))
        return 1;

    term.sa_handler = SignalDaemon::termSignalHandler;
    sigemptyset(&term.sa_mask);
    term.sa_flags = 0;
    term.sa_flags |= SA_RESTART;

    if (sigaction(SIGTERM, &term, 0))
        return 2;

    return 0;
}
#endif

int requestCaptureAndWait(const CaptureRequest& req)
{
    Flameshot* flameshot = Flameshot::instance();
    flameshot->requestCapture(req);
    QObject::connect(flameshot, &Flameshot::captureTaken, [&](const QPixmap&) {
#if defined(Q_OS_MACOS)
        // Only useful on MacOS because each instance hosts its own widgets
        if (!FlameshotDaemon::isThisInstanceHostingWidgets()) {
            qApp->exit(0);
        }
#else
        // if this instance is not daemon, make sure it exit after caputre finish
        if (FlameshotDaemon::instance() == nullptr && !Flameshot::instance()->haveExternalWidget()) {
            qApp->exit(0);
        }
#endif
    });
    QObject::connect(flameshot, &Flameshot::captureFailed, []() {
        AbstractLogger::Target logTarget = static_cast<AbstractLogger::Target>(
          ConfigHandler().showAbortNotification()
            ? AbstractLogger::Target::Default
            : AbstractLogger::Target::Default &
                ~AbstractLogger::Target::Notification);
        AbstractLogger::info(logTarget) << "Screenshot aborted.";
        qApp->exit(1);
    });
    return qApp->exec();
}

QSharedMemory* guiMutexLock()
{
    QString key = "org.flameshot.Flameshot-" APP_VERSION;
    auto* shm = new QSharedMemory(key);
#ifdef Q_OS_UNIX
    // Destroy shared memory if the last instance crashed on Unix
    shm->attach();
    delete shm;
    shm = new QSharedMemory(key);
#endif
    if (!shm->create(1)) {
        return nullptr;
    }
    return shm;
}

QTranslator translator, qtTranslator;

void configureApp(bool gui)
{
    if (gui) {
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        QApplication::setStyle("Fusion"); // Supports dark scheme on Win 10/11
#else
        QApplication::setStyle(new StyleOverride);
#endif
    }

    bool foundTranslation;
    // Configure translations
    for (const QString& path : PathInfo::translationsPaths()) {
        foundTranslation =
          translator.load(QLocale(),
                          QStringLiteral("Internationalization"),
                          QStringLiteral("_"),
                          path);
        if (foundTranslation) {
            break;
        }
    }
    if (!foundTranslation) {
        QLocale l;
        qWarning() << QStringLiteral("No Flameshot translation found for %1")
                        .arg(l.uiLanguages().join(", "));
    }

    foundTranslation =
      qtTranslator.load(QLocale::system(),
                        "qt",
                        "_",
                        QLibraryInfo::path(QLibraryInfo::TranslationsPath));
    if (!foundTranslation) {
        qWarning() << QStringLiteral("No Qt translation found for %1")
                        .arg(QLocale::languageToString(
                          QLocale::system().language()));
    }

    auto app = QCoreApplication::instance();
    app->installTranslator(&translator);
    app->installTranslator(&qtTranslator);
    app->setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);
}

// TODO find a way so we don't have to do this
/// Recreate the application as a QApplication
void reinitializeAsQApplication(int& argc, char* argv[])
{
    delete QCoreApplication::instance();
    new QApplication(argc, argv);
    configureApp(true);
}

int main(int argc, char* argv[])
{
    // Required for saving button list QList<CaptureTool::Type>
    qRegisterMetaType<QList<int>>();

    QCoreApplication::setApplicationVersion(APP_VERSION);
    QCoreApplication::setApplicationName(QStringLiteral("flameshot"));
    QCoreApplication::setOrganizationName(QStringLiteral("flameshot"));

    // no arguments, just launch Flameshot
    if (argc == 1) {
        QApplication app(argc, argv);

#ifdef USE_KDSINGLEAPPLICATION
#ifdef Q_OS_UNIX
        setup_unix_signal_handlers();
        auto signalDaemon = SignalDaemon();
#endif
        auto kdsa = KDSingleApplication(QStringLiteral("flameshot"));

        if (!kdsa.isPrimaryInstance()) {
            return 0; // Quit
        }
#endif

        configureApp(true);
        auto c = Flameshot::instance();
        FlameshotDaemon::start();

#if !(defined(Q_OS_MACOS) || defined(Q_OS_WIN))
        new FlameshotDBusAdapter(c);
        QDBusConnection dbus = QDBusConnection::sessionBus();
        if (!dbus.isConnected()) {
            AbstractLogger::error()
              << QObject::tr("Unable to connect via DBus");
        }
        dbus.registerObject(QStringLiteral("/"), c);
        dbus.registerService(QStringLiteral("org.flameshot.Flameshot"));
#endif
        return qApp->exec();
    }

    /*--------------|
     * CLI parsing  |
     * ------------*/
    new QCoreApplication(argc, argv);
    configureApp(false);
    CommandLineParser parser;
    // Add description
    parser.setDescription(
      QObject::tr("Powerful yet simple to use screenshot software."));
    parser.setGeneralErrorMessage(QObject::tr("See") + " flameshot --help.");
    // Arguments
    CommandArgument fullArgument(
      QStringLiteral("full"),
      QObject::tr("Capture screenshot of all monitors at the same time."));
    CommandArgument launcherArgument(QStringLiteral("launcher"),
                                     QObject::tr("Open the capture launcher."));
    CommandArgument guiArgument(
      QStringLiteral("gui"),
      QObject::tr("Start a manual capture in GUI mode."));
    CommandArgument configArgument(QStringLiteral("config"),
                                   QObject::tr("Configure") + " flameshot.");
    CommandArgument screenArgument(
      QStringLiteral("screen"),
      QObject::tr("Capture a screenshot of the specified monitor."));

    // Options
    CommandOption pathOption(
      { "p", "path" },
      QObject::tr("Existing directory or new file to save to"),
      QStringLiteral("path"));
    CommandOption clipboardOption(
      { "c", "clipboard" }, QObject::tr("Save the capture to the clipboard"));
    CommandOption pinOption("pin",
                            QObject::tr("Pin the capture to the screen"));
    CommandOption uploadOption({ "u", "upload" },
                               QObject::tr("Upload screenshot"));
    CommandOption delayOption({ "d", "delay" },
                              QObject::tr("Delay time in milliseconds"),
                              QStringLiteral("milliseconds"));

    CommandOption useLastRegionOption(
      "last-region",
      QObject::tr("Repeat screenshot with previously selected region"));

    CommandOption regionOption("region",
                               QObject::tr("Screenshot region to select"),
                               QStringLiteral("WxH+X+Y or string"));
    CommandOption filenameOption({ "f", "filename" },
                                 QObject::tr("Set the filename pattern"),
                                 QStringLiteral("pattern"));
    CommandOption acceptOnSelectOption(
      { "s", "accept-on-select" },
      QObject::tr("Accept capture as soon as a selection is made"));
    CommandOption trayOption({ "t", "trayicon" },
                             QObject::tr("Enable or disable the trayicon"),
                             QStringLiteral("bool"));
    CommandOption autostartOption(
      { "a", "autostart" },
      QObject::tr("Enable or disable run at startup"),
      QStringLiteral("bool"));
    CommandOption notificationOption(
      { "n", "notifications" },
      QObject::tr("Enable or disable the notifications"),
      QStringLiteral("bool"));
    CommandOption checkOption(
      "check", QObject::tr("Check the configuration for errors"));
    CommandOption showHelpOption(
      { "s", "showhelp" },
      QObject::tr("Show the help message in the capture mode"),
      QStringLiteral("bool"));
    CommandOption mainColorOption({ "m", "maincolor" },
                                  QObject::tr("Define the main UI color"),
                                  QStringLiteral("color-code"));
    CommandOption contrastColorOption(
      { "k", "contrastcolor" },
      QObject::tr("Define the contrast UI color"),
      QStringLiteral("color-code"));
    CommandOption rawImageOption({ "r", "raw" },
                                 QObject::tr("Print raw PNG capture"));
    CommandOption selectionOption(
      { "g", "print-geometry" },
      QObject::tr("Print geometry of the selection in the format WxH+X+Y. Does "
                  "nothing if raw is specified"));
    CommandOption screenNumberOption(
      { "n", "number" },
      QObject::tr("Define the screen to capture (starting from 0)") + ",\n" +
        QObject::tr("default: screen containing the cursor"),
      QObject::tr("Screen number"),
      QStringLiteral("-1"));

    // Add checkers
    auto colorChecker = [](const QString& colorCode) -> bool {
        QColor parsedColor(colorCode);
        return parsedColor.isValid() && parsedColor.alphaF() == 1.0;
    };
    QString colorErr =
      QObject::tr("Invalid color, "
                  "this flag supports the following formats:\n"
                  "- #RGB (each of R, G, and B is a single hex digit)\n"
                  "- #RRGGBB\n- #RRRGGGBBB\n"
                  "- #RRRRGGGGBBBB\n"
                  "- Named colors like 'blue' or 'red'\n"
                  "You may need to escape the '#' sign as in '\\#FFF'");

    const QString delayErr =
      QObject::tr("Invalid delay, it must be a number greater than 0");
    const QString numberErr =
      QObject::tr("Invalid screen number, it must be non negative");
    const QString regionErr = QObject::tr(
      "Invalid region, use 'WxH+X+Y' or 'all' or 'screen0/screen1/...'.");
    auto numericChecker = [](const QString& delayValue) -> bool {
        bool ok;
        int value = delayValue.toInt(&ok);
        return ok && value >= 0;
    };
    auto regionChecker = [](const QString& region) -> bool {
        Region valueHandler;
        return valueHandler.check(region);
    };

    const QString pathErr =
      QObject::tr("Invalid path, must be an existing directory or a new file "
                  "in an existing directory");
    auto pathChecker = [pathErr](const QString& pathValue) -> bool {
        QFileInfo fileInfo(pathValue);
        if (fileInfo.isDir() || fileInfo.dir().exists()) {
            return true;
        } else {
            AbstractLogger::error() << QObject::tr(pathErr.toLatin1().data());
            return false;
        }
    };

    const QString booleanErr =
      QObject::tr("Invalid value, it must be defined as 'true' or 'false'");
    auto booleanChecker = [](const QString& value) -> bool {
        return value == QLatin1String("true") ||
               value == QLatin1String("false");
    };

    contrastColorOption.addChecker(colorChecker, colorErr);
    mainColorOption.addChecker(colorChecker, colorErr);
    delayOption.addChecker(numericChecker, delayErr);
    regionOption.addChecker(regionChecker, regionErr);
    useLastRegionOption.addChecker(booleanChecker, booleanErr);
    pathOption.addChecker(pathChecker, pathErr);
    trayOption.addChecker(booleanChecker, booleanErr);
    autostartOption.addChecker(booleanChecker, booleanErr);
    notificationOption.addChecker(booleanChecker, booleanErr);
    showHelpOption.addChecker(booleanChecker, booleanErr);
    screenNumberOption.addChecker(numericChecker, numberErr);

    // Relationships
    parser.AddArgument(guiArgument);
    parser.AddArgument(screenArgument);
    parser.AddArgument(fullArgument);
    parser.AddArgument(launcherArgument);
    parser.AddArgument(configArgument);
    auto helpOption = parser.addHelpOption();
    auto versionOption = parser.addVersionOption();
    parser.AddOptions({ pathOption,
                        clipboardOption,
                        delayOption,
                        regionOption,
                        useLastRegionOption,
                        rawImageOption,
                        selectionOption,
                        uploadOption,
                        pinOption,
                        acceptOnSelectOption },
                      guiArgument);
    parser.AddOptions({ screenNumberOption,
                        clipboardOption,
                        pathOption,
                        delayOption,
                        regionOption,
                        rawImageOption,
                        uploadOption,
                        pinOption },
                      screenArgument);
    parser.AddOptions({ pathOption,
                        clipboardOption,
                        delayOption,
                        regionOption,
                        rawImageOption,
                        uploadOption },
                      fullArgument);
    parser.AddOptions({ autostartOption,
                        notificationOption,
                        filenameOption,
                        trayOption,
                        showHelpOption,
                        mainColorOption,
                        contrastColorOption,
                        checkOption },
                      configArgument);
    // Parse
    if (!parser.parse(qApp->arguments())) {
        goto finish;
    }

    // PROCESS DATA
    //--------------
    Flameshot::setOrigin(Flameshot::CLI);
    if (parser.isSet(helpOption) || parser.isSet(versionOption)) {
    } else if (parser.isSet(launcherArgument)) { // LAUNCHER
        reinitializeAsQApplication(argc, argv);
        Flameshot* flameshot = Flameshot::instance();
        flameshot->launcher();
        qApp->exec();
    } else if (parser.isSet(guiArgument)) { // GUI
        reinitializeAsQApplication(argc, argv);
        // Prevent multiple instances of 'flameshot gui' from running if not
        // configured to do so.
        if (!ConfigHandler().allowMultipleGuiInstances()) {
            auto* mutex = guiMutexLock();
            if (!mutex) {
                return 1;
            }
            QObject::connect(
              qApp, &QCoreApplication::aboutToQuit, qApp, [mutex]() {
                  mutex->detach();
                  delete mutex;
              });
        }

        // Option values
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        QString region = parser.value(regionOption);
        bool useLastRegion = parser.isSet(useLastRegionOption);
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);
        bool printGeometry = parser.isSet(selectionOption);
        bool pin = parser.isSet(pinOption);
        bool upload = parser.isSet(uploadOption);
        bool acceptOnSelect = parser.isSet(acceptOnSelectOption);
        CaptureRequest req(CaptureRequest::GRAPHICAL_MODE, delay, path);
        if (!region.isEmpty()) {
            auto selectionRegion = Region().value(region).toRect();
            req.setInitialSelection(selectionRegion);
        } else if (useLastRegion) {
            req.setInitialSelection(getLastRegion());
        }
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (printGeometry) {
            req.addTask(CaptureRequest::PRINT_GEOMETRY);
        }
        if (pin) {
            req.addTask(CaptureRequest::PIN);
        }
        if (upload) {
            req.addTask(CaptureRequest::UPLOAD);
        }
        if (acceptOnSelect) {
            req.addTask(CaptureRequest::ACCEPT_ON_SELECT);
            if (!clipboard && !raw && path.isEmpty() && !printGeometry &&
                !pin && !upload) {
                req.addSaveTask();
            }
        }
        return requestCaptureAndWait(req);
    } else if (parser.isSet(fullArgument)) { // FULL
        reinitializeAsQApplication(argc, argv);

        // Option values
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        QString region = parser.value(regionOption);
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);
        bool upload = parser.isSet(uploadOption);
        // Not a valid command

        CaptureRequest req(CaptureRequest::FULLSCREEN_MODE, delay);
        if (!region.isEmpty()) {
            req.setInitialSelection(Region().value(region).toRect());
        }
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (upload) {
            req.addTask(CaptureRequest::UPLOAD);
        }
        if (!clipboard && path.isEmpty() && !raw && !upload) {
            req.addSaveTask();
        }
        return requestCaptureAndWait(req);
    } else if (parser.isSet(screenArgument)) { // SCREEN
        reinitializeAsQApplication(argc, argv);

        QString numberStr = parser.value(screenNumberOption);
        // Option values
        int screenNumber =
          numberStr.startsWith(QLatin1String("-")) ? -1 : numberStr.toInt();
        QString path = parser.value(pathOption);
        if (!path.isEmpty()) {
            path = QDir(path).absolutePath();
        }
        int delay = parser.value(delayOption).toInt();
        QString region = parser.value(regionOption);
        bool clipboard = parser.isSet(clipboardOption);
        bool raw = parser.isSet(rawImageOption);
        bool pin = parser.isSet(pinOption);
        bool upload = parser.isSet(uploadOption);

        CaptureRequest req(CaptureRequest::SCREEN_MODE, delay, screenNumber);
        if (!region.isEmpty()) {
            if (region.startsWith("screen")) {
                AbstractLogger::error()
                  << "The 'screen' command does not support "
                     "'--region screen<N>'.\n"
                     "See flameshot --help.\n";
                exit(1);
            }
            req.setInitialSelection(Region().value(region).toRect());
        }
        if (clipboard) {
            req.addTask(CaptureRequest::COPY);
        }
        if (raw) {
            req.addTask(CaptureRequest::PRINT_RAW);
        }
        if (!path.isEmpty()) {
            req.addSaveTask(path);
        }
        if (pin) {
            req.addTask(CaptureRequest::PIN);
        }
        if (upload) {
            req.addTask(CaptureRequest::UPLOAD);
        }

        if (!clipboard && !raw && path.isEmpty() && !pin && !upload) {
            req.addSaveTask();
        }

        return requestCaptureAndWait(req);
    } else if (parser.isSet(configArgument)) { // CONFIG
        bool autostart = parser.isSet(autostartOption);
        bool notification = parser.isSet(notificationOption);
        bool filename = parser.isSet(filenameOption);
        bool tray = parser.isSet(trayOption);
        bool mainColor = parser.isSet(mainColorOption);
        bool contrastColor = parser.isSet(contrastColorOption);
        bool check = parser.isSet(checkOption);
        bool someFlagSet = (autostart || notification || filename || tray ||
                            mainColor || contrastColor || check);
        if (check) {
            AbstractLogger err = AbstractLogger::error(AbstractLogger::Stderr);
            bool ok = ConfigHandler().checkForErrors(&err);
            if (ok) {
                AbstractLogger::info()
                  << QStringLiteral("No errors detected.\n");
                goto finish;
            } else {
                return 1;
            }
        }
        if (!someFlagSet) {
            // Open gui when no options are given
            reinitializeAsQApplication(argc, argv);
            QObject::connect(
              qApp, &QApplication::lastWindowClosed, qApp, &QApplication::quit);
            Flameshot::instance()->config();
            qApp->exec();
        } else {
            ConfigHandler config;

            if (autostart) {
                config.setStartupLaunch(parser.value(autostartOption) ==
                                        "true");
            }
            if (notification) {
                config.setShowDesktopNotification(
                  parser.value(notificationOption) == "true");
            }
            if (filename) {
                QString newFilename(parser.value(filenameOption));
                config.setFilenamePattern(newFilename);
                FileNameHandler fh;
                QTextStream(stdout)
                  << QStringLiteral("The new pattern is '%1'\n"
                                    "Parsed pattern example: %2\n")
                       .arg(newFilename, fh.parsedPattern());
            }
            if (tray) {
                config.setDisabledTrayIcon(parser.value(trayOption) == "false");
            }
            if (mainColor) {
                // TODO use value handler
                QString colorCode = parser.value(mainColorOption);
                QColor parsedColor(colorCode);
                config.setUiColor(parsedColor);
            }
            if (contrastColor) {
                QString colorCode = parser.value(contrastColorOption);
                QColor parsedColor(colorCode);
                config.setContrastUiColor(parsedColor);
            }
        }
    }
finish:

    return 0;
}
