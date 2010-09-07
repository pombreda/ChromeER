#!/usr/bin/python
# Copyright (c) 2006-2009 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Script to create Chrome Installer archive.

  This script is used to create an archive of all the files required for a
  Chrome install in appropriate directory structure. It reads chrome.release
  file as input, creates chrome.7z archive, compresses setup.exe and
  generates packed_files.txt for mini_installer project.

"""

import ConfigParser
import glob
import md5
import optparse
import os
import shutil
import sys


ARCHIVE_DIR = "installer_archive"

# suffix to uncompresed full archive file, appended to options.output_name
ARCHIVE_SUFFIX = ".7z"
BSDIFF_EXEC = "bsdiff.exe"
CHROME_DIR = "Chrome-bin"
CHROME_PATCH_FILE_SUFFIX = "_patch"  # prefixed by options.output_name

# compressed full archive suffix, will be prefixed by options.output_name
COMPRESSED_ARCHIVE_SUFFIX = ".packed.7z"

COMPRESSED_FILE_EXT = ".packed.7z"     # extension of patch archive file
COURGETTE_EXEC = "courgette.exe"
MINI_INSTALLER_INPUT_FILE = "packed_files.txt"
PACKED_FILE_COMMENTS = """
// This file is automatically generated by create_installer_archive.py.
// It contains the resource entries that are going to be linked inside
// mini_installer.exe. For each file to be linked there should be two
// lines:
// - The first line contains the output filename (without path) and the
// type of the resource ('BN' - not compressed , 'BL' - LZ compressed,
// 'B7' - LZMA compressed)
// - The second line contains the path to the input file. Uses '/' to
// separate path components.
"""
PATCH_FILE_EXT = '.diff'
SETUP_EXEC = "setup.exe"
SETUP_PATCH_FILE_PREFIX = "setup_patch"
TEMP_ARCHIVE_DIR = "temp_installer_archive"
VERSION_FILE = "VERSION"

def BuildVersion(output_dir):
  """Returns the full build version string constructed from information in
  VERSION_FILE.  Any segment not found in that file will default to '0'.
  """
  major = 0
  minor = 0
  build = 0
  patch = 0
  for line in open(os.path.join(output_dir, "..", "..", "chrome", VERSION_FILE), 'r'):
    line = line.rstrip()
    if line.startswith('MAJOR='):
      major = line[6:]
    elif line.startswith('MINOR='):
      minor = line[6:]
    elif line.startswith('BUILD='):
      build = line[6:]
    elif line.startswith('PATCH='):
      patch = line[6:]
  return '%s.%s.%s.%s' % (major, minor, build, patch)

def CompressUsingLZMA(output_dir, compressed_file, input_file):
  lzma_exec = GetLZMAExec(output_dir)
  cmd = '%s a -t7z "%s" "%s" -mx9' % (lzma_exec, compressed_file, input_file)
  if os.path.exists(compressed_file):
    os.remove(compressed_file)
  RunSystemCommand(cmd)

def CopyAllFilesToStagingDir(config, distribution, staging_dir, output_dir):
  """Copies the files required for installer archive.
  Copies all common files required for various distributions of Chromium and
  also files for the specific Chromium build specified by distribution.
  """
  CopySectionFilesToStagingDir(config, 'GENERAL', staging_dir, output_dir)
  if distribution:
    if len(distribution) > 1 and distribution[0] == '_':
      distribution = distribution[1:]
    CopySectionFilesToStagingDir(config, distribution.upper(),
                                 staging_dir, output_dir)

def IsChromeFrameFile(file):
  for cf_file in ['npchrome_frame', 'chrome_launcher']:
    if file.lower().find(cf_file) != -1:
      return True
  return False

def CopySectionFilesToStagingDir(config, section, staging_dir, output_dir):
  """Copies installer archive files specified in section to staging dir.
  This method copies reads section from config file and copies all the files
  specified to staging dir.
  """
  for option in config.options(section):
    if option.endswith('dir'):
      continue

    dst = os.path.join(staging_dir, config.get(section, option))
    if not os.path.exists(dst):
      os.makedirs(dst)
    for file in glob.glob(os.path.join(output_dir, option)):
      if IsChromeFrameFile(file):
        try:
          shutil.copy(file, dst)
        except IOError:
          # TODO(robertshield): Temporary hack to work around problems building
          # Chrome Frame binaries on non-Chrome Frame builders. Remove this
          # asap.
          print 'Error attempting to copy ' + file + ' to ' + dst
      else:
        shutil.copy(file, dst)

def GenerateDiffPatch(options, orig_file, new_file, patch_file):
  if (options.diff_algorithm == "COURGETTE"):
    exe_file = os.path.join(options.last_chrome_installer, COURGETTE_EXEC)
    cmd = '%s -gen "%s" "%s" "%s"' % (exe_file, orig_file, new_file, patch_file)
  else:
    exe_file = os.path.join(options.output_dir, BSDIFF_EXEC)
    cmd = '%s "%s" "%s" "%s"' % (exe_file, orig_file, new_file, patch_file)
  RunSystemCommand(cmd)

def GetLZMAExec(output_dir):
  lzma_exec = os.path.join(output_dir, "..", "..", "third_party",
                           "lzma_sdk", "Executable", "7za.exe")
  return lzma_exec

def GetPrevVersion(output_dir, temp_dir, last_chrome_installer):
  if not last_chrome_installer:
    return ''

  lzma_exec = GetLZMAExec(options.output_dir)
  prev_archive_file = os.path.join(options.last_chrome_installer,
                                   options.output_name + ARCHIVE_SUFFIX)
  cmd = '%s x -o"%s" "%s" Chrome-bin/*/gears.dll' % (lzma_exec, temp_dir,
                                                       prev_archive_file)
  RunSystemCommand(cmd)
  dll_path = glob.glob(os.path.join(temp_dir, 'Chrome-bin', '*', 'gears.dll'))
  return os.path.split(os.path.split(dll_path[0])[0])[1]

def MakeStagingDirectories(output_dir):
  """Creates a staging path for installer archive. If directory exists already,
  deletes the existing directory.
  """
  prefixed_archive_dir = (options.archive_prefix or "") + ARCHIVE_DIR
  file_path = os.path.join(output_dir, prefixed_archive_dir)
  if os.path.exists(file_path):
    shutil.rmtree(file_path)
  os.makedirs(file_path)

  prefixed_temp_archive_dir = (options.archive_prefix or "") + TEMP_ARCHIVE_DIR
  temp_file_path = os.path.join(output_dir, prefixed_temp_archive_dir)
  if os.path.exists(temp_file_path):
    shutil.rmtree(temp_file_path)
  os.makedirs(temp_file_path)
  return (file_path, temp_file_path)

def Readconfig(output_dir, input_file, current_version):
  """Reads config information from input file after setting default value of
  global variabes.
  """
  variables = {}
  variables['ChromeDir'] = CHROME_DIR
  variables['VersionDir'] = os.path.join(variables['ChromeDir'],
                                          current_version)
  config = ConfigParser.SafeConfigParser(variables)
  config.read(input_file)
  return config

def RunSystemCommand(cmd):
  print 'Running [' + cmd + ']'
  if (os.system(cmd) != 0):
    raise "Error while running cmd: %s" % cmd

def CreateArchiveFile(options, staging_dir, current_version, prev_version):
  """Creates a new installer archive file after deleting any existing old file.
  """
  # First create an uncompressed archive file for the current build (chrome.7z)
  lzma_exec = GetLZMAExec(options.output_dir)
  archive_file = os.path.join(options.output_dir,
                              options.output_name + ARCHIVE_SUFFIX)
  cmd = '%s a -t7z "%s" "%s" -mx0' % (lzma_exec, archive_file,
                                      os.path.join(staging_dir, CHROME_DIR))
  # There doesnt seem to be any way in 7za.exe to override existing file so
  # we always delete before creating a new one.
  if not os.path.exists(archive_file):
    RunSystemCommand(cmd)
  elif options.skip_rebuild_archive != "true":
    os.remove(archive_file)
    RunSystemCommand(cmd)

  # If we are generating a patch, run bsdiff against previous build and
  # compress the resulting patch file. If this is not a patch just compress the
  # uncompressed archive file.
  patch_name_prefix = options.output_name + CHROME_PATCH_FILE_SUFFIX
  if options.last_chrome_installer:
    prev_archive_file = os.path.join(options.last_chrome_installer,
                                     options.output_name + ARCHIVE_SUFFIX)
    patch_file = os.path.join(options.output_dir, patch_name_prefix +
                                                  PATCH_FILE_EXT)
    GenerateDiffPatch(options, prev_archive_file, archive_file, patch_file)
    compressed_archive_file = patch_name_prefix + '_' + \
                              current_version + '_from_' + prev_version + \
                              COMPRESSED_FILE_EXT
    orig_file = patch_file
  else:
    compressed_archive_file = options.output_name + COMPRESSED_ARCHIVE_SUFFIX
    orig_file = archive_file

  compressed_archive_file_path = os.path.join(options.output_dir,
                                              compressed_archive_file)
  CompressUsingLZMA(options.output_dir, compressed_archive_file_path, orig_file)

  return compressed_archive_file


def PrepareSetupExec(options, staging_dir, current_version, prev_version):
  """Prepares setup.exe for bundling in mini_installer based on options."""
  if options.setup_exe_format == "FULL":
    setup_file = SETUP_EXEC
  elif options.setup_exe_format == "DIFF":
    if not options.last_chrome_installer:
      raise "To use DIFF for setup.exe, --last_chrome_installer is needed."
    prev_setup_file = os.path.join(options.last_chrome_installer, SETUP_EXEC)
    new_setup_file = os.path.join(options.output_dir, SETUP_EXEC)
    patch_file = os.path.join(options.output_dir, SETUP_PATCH_FILE_PREFIX +
                                                  PATCH_FILE_EXT)
    GenerateDiffPatch(options, prev_setup_file, new_setup_file, patch_file)
    setup_file = SETUP_PATCH_FILE_PREFIX + '_' + current_version + \
                 '_from_' + prev_version + COMPRESSED_FILE_EXT
    setup_file_path = os.path.join(options.output_dir, setup_file)
    CompressUsingLZMA(options.output_dir, setup_file_path, patch_file)
  else:
    cmd = 'makecab.exe /D CompressionType=LZX /V1 /L "%s" "%s"' % (
        options.output_dir, os.path.join(options.output_dir, SETUP_EXEC))
    RunSystemCommand(cmd)
    setup_file = SETUP_EXEC[:len(SETUP_EXEC) - 1] + "_"
  return setup_file


def CreateResourceInputFile(output_dir, setup_format, archive_file, setup_file):
  """Creates resource input file (packed_files.txt) for mini_installer project.

  This method checks the format of setup.exe being used and according sets
  its resource type.
  """
  setup_resource_type = "BL"
  if (options.setup_exe_format == "FULL"):
    setup_resource_type = "BN"
  elif (options.setup_exe_format == "DIFF"):
    setup_resource_type = "B7"
  setup_file_entry = "%s\t\t%s\n\"%s\"" % (setup_file, setup_resource_type,
      os.path.join(output_dir, setup_file).replace("\\","/"))

  archive_file_entry = "\n%s\t\tB7\n\"%s\"\n" % (archive_file,
      os.path.join(output_dir, archive_file).replace("\\","/"))
  output_file = os.path.join(output_dir, MINI_INSTALLER_INPUT_FILE)
  f = open(output_file, 'w')
  try:
    f.write(PACKED_FILE_COMMENTS)
    f.write(setup_file_entry)
    f.write(archive_file_entry)
  finally:
    f.close()


def main(options):
  """Main method that reads input file, creates archive file and write
  resource input file.
  """
  current_version = BuildVersion(options.output_dir)

  config = Readconfig(options.output_dir, options.input_file, current_version)

  (staging_dir, temp_dir) = MakeStagingDirectories(options.output_dir)

  prev_version = GetPrevVersion(options.output_dir, temp_dir,
                                options.last_chrome_installer)

  CopyAllFilesToStagingDir(config, options.distribution,
                           staging_dir, options.output_dir)

  version_numbers = current_version.split('.')
  current_build_number = version_numbers[2] + '.' + version_numbers[3]
  prev_build_number = ''
  if prev_version:
    version_numbers = prev_version.split('.')
    prev_build_number = version_numbers[2] + '.' + version_numbers[3]

  # Name of the archive file built (for example - chrome.7z or
  # patch-<old_version>-<new_version>.7z or patch-<new_version>.7z
  archive_file = CreateArchiveFile(options, staging_dir,
                                   current_build_number, prev_build_number)

  setup_file = PrepareSetupExec(options, staging_dir,
                                current_build_number, prev_build_number)

  CreateResourceInputFile(options.output_dir, options.setup_exe_format,
                          archive_file, setup_file)


if '__main__' == __name__:
  option_parser = optparse.OptionParser()
  option_parser.add_option('-o', '--output_dir', help='Output directory')
  option_parser.add_option('-i', '--input_file', help='Input file')
  option_parser.add_option('-d', '--distribution',
      help='Name of Chromium Distribution. Optional.')
  option_parser.add_option('-s', '--skip_rebuild_archive',
      default="False", help='Skip re-building Chrome.7z archive if it exists.')
  option_parser.add_option('-l', '--last_chrome_installer',
      help='Generate differential installer. The value of this parameter ' +
           'specifies the directory that contains base versions of ' +
           'setup.exe, courgette.exe (if --diff_algorithm is COURGETTE) ' +
           '& chrome.7z.')
  option_parser.add_option('-p', '--archive_prefix',
      help='Specifies a prefix to the archive path. Useful if building ' +
           'multiple installer archives.')
  option_parser.add_option('-f', '--setup_exe_format', default='COMPRESSED',
      help='How setup.exe should be included {COMPRESSED|DIFF|FULL}.')
  option_parser.add_option('-a', '--diff_algorithm', default='BSDIFF',
      help='Diff algorithm to use when generating differential patches ' +
           '{BSDIFF|COURGETTE}.')
  option_parser.add_option('-n', '--output_name', default='chrome',
      help='Name used to prefix names of generated archives.')

  options, args = option_parser.parse_args()
  print sys.argv
  sys.exit(main(options))
