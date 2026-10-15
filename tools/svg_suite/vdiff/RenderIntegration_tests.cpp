// Exercise official Qt Render signals and export with a real Donner executable.
// Image conformance remains the native GoogleTest/pixelmatch suite's job.
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QImageReader>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>

#include "exportdialog.h"
#include "render.h"

namespace {
bool check(bool condition, const QString& message) {
  if (!condition) {
    qCritical().noquote() << message;
  }
  return condition;
}

struct Observation {
  QImage actual;
  QImage diff;
  QString error;
  bool finished = false;
};

void observe(Render& render, QEventLoop& loop, Observation& result) {
  QObject::connect(&render, &Render::imageReady, &loop, [&](Backend backend, const QImage& image) {
    if (backend == Backend::Donner) {
      result.actual = image;
    }
  });
  QObject::connect(&render, &Render::diffReady, &loop, [&](Backend backend, const QImage& image) {
    if (backend == Backend::Donner) {
      result.diff = image;
    }
  });
  QObject::connect(&render, &Render::renderError, &loop,
                   [&](Backend backend, const QString& error) {
                     if (backend == Backend::Donner) {
                       result.error = error;
                     }
                   });
  QObject::connect(&render, &Render::finished, &loop, [&]() {
    result.finished = true;
    loop.quit();
  });
}

bool verifyImages(const Observation& result, const QString& input, const QString& artifacts,
                  int previewSize, bool expectError) {
  if (!check(result.finished, "Official Render did not finish within the timeout") ||
      !check(!result.actual.isNull() && !result.diff.isNull(),
             "Missing Donner image or diff signal") ||
      !check(expectError ? result.error.contains("executable") : result.error.isEmpty(),
             "Unexpected Donner error status")) {
    return false;
  }
  const QFileInfo source(input);
  const QSize canonical =
      QImageReader(source.absolutePath() + "/" + source.completeBaseName() + ".png").size();
  if (!check(!canonical.isEmpty() && canonical.width() != canonical.height(),
             "Use a valid non-square reference fixture")) {
    return false;
  }
  const QSize preview = canonical.scaled(previewSize, previewSize, Qt::KeepAspectRatio);
  const QSize expected = expectError ? QSize(previewSize, previewSize) : preview;
  const QString tag = QString::number(previewSize) + (expectError ? "-error" : "-render");
  if (!check(result.actual.size() == expected, "Unexpected Donner preview dimensions") ||
      !check(result.actual.save(artifacts + "/" + tag + ".png"),
             "Cannot retain preview artifact") ||
      !check(result.diff.save(artifacts + "/" + tag + "-ui-diff.png"), "Cannot retain UI diff")) {
    return false;
  }
  qInfo().noquote() << "Official Render/Donner completed:" << tag << "canonical" << canonical
                    << "preview" << result.actual.size();
  return true;
}

bool runRender(Settings& settings, const QString& input, const QString& artifacts, int previewSize,
               bool expectError) {
  settings.viewSize = previewSize;
  Render render;
  render.setSettings(&settings);
  render.setScale(1.0);
  QEventLoop loop;
  Observation result;
  observe(render, loop, result);
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start(15000);
  render.render(input);
  loop.exec();
  return verifyImages(result, input, artifacts, previewSize, expectError);
}

bool prepareSettings(const QString& renderer, Settings& loaded) {
  Settings settings;
  settings.useChrome = settings.useFirefox = settings.useSafari = false;
  settings.useBatik = settings.useInkscape = settings.useLibrsvg = false;
  settings.useSvgNet = settings.useQtSvg = false;
  settings.useDonner = true;
  settings.donnerPath = renderer;
  settings.save();
  loaded.load();
  ExportDialog exportDialog({Backend::Donner});
  return check(loaded.useDonner && loaded.donnerPath == renderer,
               "Donner settings did not round-trip") &&
         check(exportDialog.options().backends.contains(Backend::Donner),
               "Export dialog did not select enabled Donner backend");
}
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (!check(argc == 4, "Usage: render_integration RENDERER INPUT_SVG ARTIFACT_DIRECTORY")) {
    return 1;
  }
  const QString renderer = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
  const QString input = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
  const QString artifacts = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
  QTemporaryDir configuration;
  if (!check(QFileInfo(renderer).isExecutable(), "Renderer is not executable") ||
      !check(QDir().mkpath(artifacts), "Cannot create artifact directory") ||
      !check(configuration.isValid(), "Cannot isolate settings")) {
    return 1;
  }
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configuration.path());
  app.setOrganizationName("vdiff-integration-test");
  app.setApplicationName("render-integration");
  Settings loaded;
  if (!prepareSettings(renderer, loaded) || !runRender(loaded, input, artifacts, 173, false) ||
      !runRender(loaded, input, artifacts, 263, false)) {
    return 1;
  }
  loaded.donnerPath = configuration.filePath("missing-renderer");
  if (!runRender(loaded, input, artifacts, 173, true)) {
    return 1;
  }
  qInfo() << "Donner integration: settings/export, two preview scales, explicit missing-executable "
             "error control passed";
  return 0;
}
