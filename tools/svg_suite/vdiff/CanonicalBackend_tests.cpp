// Focused canonical-output checks; no UI or legacy comparison backend is
// linked.
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

#include "donner-renderer.h"

namespace {
bool check(bool condition, const QString& message) {
  if (!condition) {
    qCritical().noquote() << message;
  }
  return condition;
}

bool writeFile(const QString& path, const QByteArray& contents) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) &&
         file.write(contents) == contents.size() && file.flush();
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QStringList arguments(const QString& input, const QString& output, const QSize& size) {
  const QString root = DonnerCorpusRoot();
  return {"--input",  input,
          "--output", output,
          "--root",   root,
          "--fonts",  root + "/fonts",
          "--width",  QString::number(size.width()),
          "--height", QString::number(size.height())};
}

bool realRenderer(const QString& input, const QString& executable, const QDir& output,
                  const QString& name, const QSize& dimensions) {
  const QString canonical = output.filePath(name + ".png");
  const auto result = RenderDonnerToFile(input, executable, canonical);
  if (!check(result.error.isEmpty() && !result.image.isNull(), "Canonical rendering failed") ||
      !check(result.image.size() == dimensions, "Canonical dimensions were scaled") ||
      !check(result.arguments == arguments(input, canonical, dimensions),
             "Backend invocation differs from the explicit canonical contract")) {
    return false;
  }
  const QString directPath = output.filePath(name + "-direct.png");
  QProcess direct;
  direct.start(executable, arguments(input, directPath, dimensions));
  if (!check(direct.waitForStarted() && direct.waitForFinished(15000) &&
                 direct.exitStatus() == QProcess::NormalExit && direct.exitCode() == 0,
             "Direct renderer control failed")) {
    return false;
  }
  const QByteArray actual = readFile(canonical);
  return check(!actual.isEmpty() && actual == readFile(directPath),
               "Official backend did not preserve the renderer's exact PNG bytes");
}

int fakeRenderer(const QStringList& args) {
  const QString mode = qEnvironmentVariable("DONNER_CANONICAL_PROBE_MODE");
  if (mode == "fail") {
    qCritical() << "controlled child failure";
    return 23;
  }
  if (mode == "hang") {
#ifdef Q_OS_UNIX
    const QJsonObject record{
        {"pid", int(getpid())}, {"parent_pid", int(getppid())}, {"process_group", int(getpgrp())}};
    if (!writeFile(qEnvironmentVariable("DONNER_CANONICAL_PROBE_RECEIPT"),
                   QJsonDocument(record).toJson(QJsonDocument::Compact))) {
      return 24;
    }
    return QCoreApplication::exec();
#else
    return 25;
#endif
  }
  const int output = args.indexOf("--output");
  if (output < 0 || output + 1 >= args.size()) {
    return 26;
  }
  if (mode == "wrong-size") {
    QImage image(1, 1, QImage::Format_RGBA8888);
    image.fill(Qt::red);
    return image.save(args.at(output + 1)) ? 0 : 27;
  }
  return mode == "no-output" ? 0 : 28;
}

bool rejected(const QString& input, const QString& renderer, const QString& output) {
  const auto result = RenderDonnerToFile(input, renderer, output);
  return check(!result.error.isEmpty() && result.image.isNull(),
               "Invalid rendering was not returned as an explicit error");
}

bool failureControls(const QString& input, const QString& renderer, const QDir& output,
                     const QString& missingReference) {
  const QString existing = output.filePath("existing.png");
  const QByteArray sentinel("existing-output-sentinel");
  if (!writeFile(existing, sentinel) || !rejected(input, renderer, existing) ||
      !check(readFile(existing) == sentinel, "Existing output was overwritten") ||
      !rejected(input, output.filePath("missing-executable"), output.filePath("missing.png")) ||
      !rejected(missingReference, renderer, output.filePath("missing-reference.png"))) {
    return false;
  }
  const QByteArray oldMode = qgetenv("DONNER_CANONICAL_PROBE_MODE");
  const bool wasSet = qEnvironmentVariableIsSet("DONNER_CANONICAL_PROBE_MODE");
  bool valid = true;
  for (const QByteArray& mode :
       {QByteArray("fail"), QByteArray("wrong-size"), QByteArray("no-output")}) {
    qputenv("DONNER_CANONICAL_PROBE_MODE", mode);
    valid &= rejected(input, QCoreApplication::applicationFilePath(),
                      output.filePath(QString::fromLatin1(mode) + ".png"));
  }
  if (wasSet) {
    qputenv("DONNER_CANONICAL_PROBE_MODE", oldMode);
  } else {
    qunsetenv("DONNER_CANONICAL_PROBE_MODE");
  }
  return valid;
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().value(1) == "--input") {
    return fakeRenderer(app.arguments());
  }
  if (!check(argc == 3,
             "Usage: canonical-backend-tests DONNER_RENDERER "
             "FRESH_ARTIFACT_DIRECTORY")) {
    return 1;
  }
  const QString renderer = QFileInfo(app.arguments().at(1)).absoluteFilePath();
  const QDir output(app.arguments().at(2));
  QTemporaryDir fixture(DonnerCorpusRoot() + "/canonical-check-XXXXXX");
  if (!check(fixture.isValid() && QDir().mkpath(output.path()),
             "Cannot create control directories")) {
    return 1;
  }
  const QByteArray svg(
      "<svg xmlns='http://www.w3.org/2000/svg' width='4' height='7'>"
      "<rect width='4' height='7' fill='red'/></svg>");
  const QString portrait = fixture.filePath("portrait.svg");
  const QString missingReference = fixture.filePath("missing-reference.svg");
  QImage reference(4, 7, QImage::Format_RGBA8888);
  reference.fill(Qt::red);
  if (!writeFile(portrait, svg) || !writeFile(missingReference, svg) ||
      !reference.save(fixture.filePath("portrait.png"))) {
    return 1;
  }
  const QString landscape = DonnerCorpusRoot() + "/tests/shapes/rect/percentage-values-1.svg";
  if (!realRenderer(landscape, renderer, output, "landscape", QSize(500, 250)) ||
      !realRenderer(portrait, renderer, output, "portrait", QSize(4, 7)) ||
      !failureControls(landscape, renderer, output, missingReference)) {
    return 1;
  }
  qInfo() << "Canonical backend: two exact-byte/dimension controls and six "
             "rejection controls passed";
  return 0;
}
