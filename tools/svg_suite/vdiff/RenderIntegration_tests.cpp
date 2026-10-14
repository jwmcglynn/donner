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
void require(bool condition, const QString& message) {
  if (!condition) {
    throw message;
  }
}
void runRender(Settings& settings, const QString& input, const QString& artifacts, int previewSize,
               bool expectError) {
  settings.viewSize = previewSize;
  Render render;
  render.setSettings(&settings);
  render.setScale(1.0);
  QEventLoop loop;
  QImage actual, diff;
  bool finished = false;
  QObject::connect(&render, &Render::imageReady, [&](Backend backend, const QImage& image) {
    if (backend == Backend::Donner) {
      actual = image;
    }
  });
  QObject::connect(&render, &Render::diffReady, [&](Backend backend, const QImage& image) {
    if (backend == Backend::Donner) {
      diff = image;
    }
  });
  QObject::connect(&render, &Render::finished, [&]() {
    finished = true;
    loop.quit();
  });
  QTimer timer;
  timer.setSingleShot(true);
  QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
  timer.start(15000);
  render.render(input);
  loop.exec();
  require(finished, "Official Render did not finish within the timeout");
  require(!actual.isNull(), "Missing Donner imageReady signal");
  require(!diff.isNull(), "Missing Donner diffReady signal");
  const QFileInfo source(input);
  const QSize canonical =
      QImageReader(source.absolutePath() + "/" + source.completeBaseName() + ".png").size();
  require(!canonical.isEmpty() && canonical.width() != canonical.height(),
          "Use a valid non-square reference fixture");
  const QSize preview = canonical.scaled(previewSize, previewSize, Qt::KeepAspectRatio);
  const QSize expected = expectError ? QSize(previewSize, previewSize) : preview;
  require(actual.size() == expected, QString("Donner preview: expected %1x%2, got %3x%4")
                                         .arg(expected.width())
                                         .arg(expected.height())
                                         .arg(actual.width())
                                         .arg(actual.height()));
  const QString tag = QString::number(previewSize) + (expectError ? "-error" : "-render");
  require(actual.save(artifacts + "/" + tag + ".png"), "Cannot retain preview artifact");
  require(diff.save(artifacts + "/" + tag + "-ui-diff.png"), "Cannot retain UI diff artifact");
  qInfo().noquote() << "Official Render/Donner completed:" << tag << "canonical" << canonical
                    << "preview" << actual.size();
}
}  // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    require(argc == 4, "Usage: render_integration RENDERER INPUT_SVG ARTIFACT_DIRECTORY");
    const QString renderer = QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath();
    const QString input = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
    const QString artifacts = QFileInfo(QString::fromLocal8Bit(argv[3])).absoluteFilePath();
    require(QFileInfo(renderer).isExecutable(), "Renderer is not executable");
    require(QDir().mkpath(artifacts), "Cannot create artifact directory");
    QTemporaryDir configuration;
    require(configuration.isValid(), "Cannot isolate settings");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, configuration.path());
    app.setOrganizationName("vdiff-integration-test");
    app.setApplicationName("render-integration");
    Settings settings;
    settings.useChrome = settings.useFirefox = settings.useSafari = false;
    settings.useBatik = settings.useInkscape = settings.useLibrsvg = false;
    settings.useSvgNet = settings.useQtSvg = false;
    settings.useDonner = true;
    settings.donnerPath = renderer;
    settings.save();
    Settings loaded;
    loaded.load();
    require(loaded.useDonner && loaded.donnerPath == renderer,
            "Donner settings did not round-trip");
    ExportDialog exportDialog({Backend::Donner});
    require(exportDialog.options().backends.contains(Backend::Donner),
            "Export dialog did not select enabled Donner backend");
    runRender(loaded, input, artifacts, 173, false);
    runRender(loaded, input, artifacts, 263, false);
    loaded.donnerPath = configuration.filePath("missing-renderer");
    runRender(loaded, input, artifacts, 173, true);
    qInfo() << "Donner integration: settings/export, two preview scales, missing-executable "
               "control passed";
    return 0;
  } catch (const QString& message) {
    qCritical().noquote() << message;
    return 1;
  }
}
