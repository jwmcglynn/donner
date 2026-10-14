// Exercise the patched official CSV implementation, without the UI.
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "tests.h"

namespace {
void require(bool condition, const QString& message) {
  if (!condition) {
    throw message;
  }
}
void writeFile(const QString& path, const QByteArray& data) {
  QFile file(path);
  require(file.open(QFile::WriteOnly), "Cannot open fixture: " + path);
  require(file.write(data) == data.size(), "Cannot write fixture: " + path);
}
QByteArray readFile(const QString& path) {
  QFile file(path);
  require(file.open(QFile::ReadOnly), "Cannot read output: " + path);
  return file.readAll();
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create fixture directory");
    require(QDir(directory.path()).mkpath("category/feature"), "Cannot create fixture tree");
    const QString input = directory.filePath("input.csv");
    const QString output = directory.filePath("output.csv");
    const QByteArray header =
        "title,chrome,firefox,safari,resvg,batik,inkscape,librsvg,svgnet,qtsvg";
    const QByteArray row = "category/feature/example.svg,1,2,3,0,1,2,3,0,1";
    const QByteArray legacy = header + "\n" + row + "\n";
    writeFile(input, legacy);
    auto old = Tests::load(TestSuite::Own, input, directory.path());
    require(old.size() == 1, "Legacy CSV did not load exactly one row");
    require(old.at(0).state.value(Backend::Firefox) == TestState::Failed,
            "Legacy Firefox column shifted");
    require(!old.at(0).state.contains(Backend::Donner),
            "Legacy CSV fabricated a Donner disposition");
    old.save(output);
    require(readFile(output) == legacy, "Legacy round-trip changed existing columns");
    old.at(0).state.insert(Backend::Donner, TestState::Crashed);
    old.save(output);
    const QByteArray extended = header + ",donner\n" + row + ",3\n";
    require(readFile(output) == extended, "Donner column was not appended accurately");
    auto added = Tests::load(TestSuite::Own, output, directory.path());
    require(added.at(0).state.value(Backend::Donner) == TestState::Crashed,
            "Extended CSV lost the Donner disposition");
    added.save(input);
    require(readFile(input) == extended, "Extended CSV round-trip changed data");
    for (const QByteArray& bad : {header + "\n" + row + ",4\n", header + "\n" + row + ",1,2\n"}) {
      writeFile(input, bad);
      bool rejected = false;
      try {
        Tests::load(TestSuite::Own, input, directory.path());
      } catch (const QString&) {
        rejected = true;
      }
      require(rejected, "Malformed extended CSV was accepted");
    }
    qInfo() << "CSV compatibility: legacy/extended round-trips and malformed data passed";
    return 0;
  } catch (const QString& message) {
    qCritical().noquote() << message;
    return 1;
  }
}
