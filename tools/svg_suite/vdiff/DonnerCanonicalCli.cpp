// A headless caller of the same canonical Donner backend used by the official
// Qt viewer.
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "donner-renderer.h"

namespace {
struct Options {
  QString input;
  QString output;
  QString renderer;
  QString root;
};

bool parse(const QStringList& arguments, Options& options) {
  for (int i = 1; i < arguments.size(); i += 2) {
    if (i + 1 >= arguments.size()) {
      return false;
    }
    const QString& flag = arguments.at(i);
    QString* destination = nullptr;
    if (flag == "--input") {
      destination = &options.input;
    } else if (flag == "--output") {
      destination = &options.output;
    } else if (flag == "--donner-renderer") {
      destination = &options.renderer;
    } else if (flag == "--root") {
      destination = &options.root;
    }
    if (!destination || !destination->isEmpty() || arguments.at(i + 1).isEmpty()) {
      return false;
    }
    *destination = arguments.at(i + 1);
  }
  return !options.input.isEmpty() && !options.output.isEmpty() && !options.renderer.isEmpty() &&
         !options.root.isEmpty();
}

bool writeEvidence(const QString& path, const QByteArray& data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly | QIODevice::NewOnly) && file.write(data) == data.size() &&
         file.flush();
}

bool retainChildEvidence(const Options& options, const DonnerRenderOutcome& result) {
  const QDir directory = QFileInfo(options.output).absoluteDir();
  const QJsonObject record{{"backend", "official-vdiff-donner"},
                           {"executable", options.renderer},
                           {"arguments", QJsonArray::fromStringList(result.arguments)},
                           {"error", result.error},
                           {"canonical_width", result.image.width()},
                           {"canonical_height", result.image.height()},
                           {"diagnostics_file", "donner-child.log"}};
  return writeEvidence(directory.filePath("donner-child.log"), result.diagnostics) &&
         writeEvidence(directory.filePath("donner-child.json"),
                       QJsonDocument(record).toJson(QJsonDocument::Indented));
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  Options options;
  if (!parse(app.arguments(), options)) {
    qCritical() << "Usage: qt-donner-canonical --input SVG --output PNG --root CORPUS "
                   "--donner-renderer EXECUTABLE";
    return 2;
  }
  if (DonnerCorpusRoot().isEmpty() ||
      QFileInfo(options.root).canonicalFilePath() != DonnerCorpusRoot()) {
    qCritical() << "Requested corpus root differs from the official backend's "
                   "compiled root.";
    return 2;
  }
  const auto result = RenderDonnerToFile(options.input, options.renderer, options.output);
  if (!retainChildEvidence(options, result)) {
    qCritical() << "Cannot retain fresh child invocation evidence.";
    return 2;
  }
  if (!result.error.isEmpty()) {
    qCritical().noquote() << result.error;
    return 1;
  }
  return 0;
}
