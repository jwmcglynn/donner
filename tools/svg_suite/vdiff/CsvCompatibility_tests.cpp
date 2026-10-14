// Exercise the patched official CSV implementation, without the UI.
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include "tests.h"

namespace {
bool check(bool condition, const QString& message) {
  if (!condition) {
    qCritical().noquote() << message;
  }
  return condition;
}

bool writeFile(const QString& path, const QByteArray& data) {
  QFile file(path);
  return check(file.open(QFile::WriteOnly), "Cannot open fixture") &&
         check(file.write(data) == data.size() && file.flush(), "Cannot write fixture");
}

bool matchesFile(const QString& path, const QByteArray& expected) {
  QFile file(path);
  if (!check(file.open(QFile::ReadOnly), "Cannot read output")) {
    return false;
  }
  const QByteArray actual = file.readAll();
  return check(file.error() == QFile::NoError && actual == expected,
               "CSV output does not match expected bytes");
}

bool roundTrips(const QTemporaryDir& directory, const QByteArray& header, const QByteArray& row) {
  const QString input = directory.filePath("input.csv");
  const QString output = directory.filePath("output.csv");
  const QByteArray legacy = header + "\n" + row + "\n";
  if (!writeFile(input, legacy)) {
    return false;
  }
  auto old = Tests::load(TestSuite::Own, input, directory.path());
  if (!check(old.error().isEmpty() && old.size() == 1, "Legacy CSV did not load one valid row") ||
      !check(old.at(0).state.value(Backend::Firefox) == TestState::Failed,
             "Legacy Firefox column shifted") ||
      !check(!old.at(0).state.contains(Backend::Donner), "Legacy CSV fabricated Donner state") ||
      !check(old.save(output).isEmpty(), "Legacy CSV save failed") ||
      !matchesFile(output, legacy)) {
    return false;
  }
  old.at(0).state.insert(Backend::Donner, TestState::Crashed);
  const QByteArray extended = header + ",donner\n" + row + ",3\n";
  if (!check(old.save(output).isEmpty(), "Extended CSV save failed") ||
      !matchesFile(output, extended)) {
    return false;
  }
  auto added = Tests::load(TestSuite::Own, output, directory.path());
  return check(added.error().isEmpty() && added.size() == 1, "Extended CSV load failed") &&
         check(added.at(0).state.value(Backend::Donner) == TestState::Crashed,
               "Extended CSV lost Donner state") &&
         check(added.save(input).isEmpty(), "Extended CSV round-trip save failed") &&
         matchesFile(input, extended) &&
         check(!added.save(directory.path()).isEmpty(), "Saving onto a directory was accepted");
}

bool malformedInputs(const QTemporaryDir& directory, const QByteArray& header,
                     const QByteArray& row) {
  const QString input = directory.filePath("invalid.csv");
  for (const QByteArray& suffix : {QByteArray(",4"), QByteArray(",1,2"), QByteArray(",bad")}) {
    if (!writeFile(input, header + "\n" + row + suffix + "\n")) {
      return false;
    }
    auto result = Tests::load(TestSuite::Own, input, directory.path());
    if (!check(!result.error().isEmpty() && result.size() == 0,
               "Malformed CSV was accepted or exposed partial rows") ||
        !check(!result.save(input).isEmpty(), "Rejected CSV was allowed to overwrite its input") ||
        !matchesFile(input, header + "\n" + row + suffix + "\n")) {
      return false;
    }
  }
  const auto missing =
      Tests::load(TestSuite::Own, directory.filePath("missing.csv"), directory.path());
  return check(!missing.error().isEmpty() && missing.size() == 0,
               "Missing CSV did not return an explicit error");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir directory;
  if (!check(directory.isValid(), "Cannot create fixture directory") ||
      !check(QDir(directory.path()).mkpath("category/feature"), "Cannot create fixture tree")) {
    return 1;
  }
  const QByteArray header = "title,chrome,firefox,safari,resvg,batik,inkscape,librsvg,svgnet,qtsvg";
  const QByteArray row = "category/feature/example.svg,1,2,3,0,1,2,3,0,1";
  if (!roundTrips(directory, header, row) || !malformedInputs(directory, header, row)) {
    return 1;
  }
  qInfo() << "CSV compatibility: two round-trips and five rejection controls passed";
  return 0;
}
