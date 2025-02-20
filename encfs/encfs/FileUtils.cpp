/*****************************************************************************
 * Author:   Valient Gough <vgough@pobox.com>
 *
 *****************************************************************************
 * Copyright (c) 2004, Valient Gough
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// defines needed for RedHat 7.3...
#ifdef linux
#define _XOPEN_SOURCE 500  // make sure pwrite() is pulled in
#endif
#define _BSD_SOURCE  // pick up setenv on RH7.3

#include <boost/version.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/binary_object.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <fcntl.h>
#include <rlog/Error.h>
#include <rlog/rlog.h>
//#include <sys/socket.h>
#include <sys/stat.h>
//#include <sys/wait.h>
#include "unistd.h"
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <list>
#include <vector>

#include "BlockNameIO.h"
#include "Cipher.h"
#include "CipherKey.h"
#include "ConfigReader.h"
#include "ConfigVar.h"
#include "Context.h"
#include "DirNode.h"
#include "FSConfig.h"
#include "FileUtils.h"
#include "Interface.h"
#include "NameIO.h"
#include "Range.h"
#include "autosprintf.h"
#include "config.h"
#include "i18n.h"
#include "intl/gettext.h"
#include "readpassphrase.h"

// disable rlog section grouping for this file.. seems to cause problems
#undef min
#undef max
#undef RLOG_SECTION
#define RLOG_SECTION

using namespace rel;
using namespace rlog;
using namespace std;
using gnu::autosprintf;
namespace fs = boost::filesystem;
namespace serial = boost::serialization;

static const int DefaultBlockSize = 1024;
// The maximum length of text passwords.  If longer are needed,
// use the extpass option, as extpass can return arbitrary length binary data.
static const int MaxPassBuf = 512;

static const int NormalKDFDuration = 500;     // 1/2 a second
static const int ParanoiaKDFDuration = 3000;  // 3 seconds

// environment variable names for values encfs stores in the environment when
// calling an external password program.
static const char ENCFS_ENV_ROOTDIR[] = "encfs_root";
static const char ENCFS_ENV_STDOUT[] = "encfs_stdout";
static const char ENCFS_ENV_STDERR[] = "encfs_stderr";

// static int V5SubVersion = 20040518;
// static int V5SubVersion = 20040621; // add external IV chaining
static int V5SubVersion = 20040813;  // fix MACFileIO block size issues
static int V5SubVersionDefault = 0;

// 20080813 was really made on 20080413 -- typo on date..
// const int V6SubVersion = 20080813; // switch to v6/XML, add allowHoles option
// const int V6SubVersion = 20080816; // add salt and iteration count
/*
 * In boost 1.42+, serial numbers change to 8 bit, which means the date
 * numbering scheme does not work any longer.
 * boost-versioning.h implements a workaround that sets the version to
 * 20 for boost 1.42+. */
const int V6SubVersion = 20100713;  // add version field for boost 1.42+

struct ConfigInfo {
  const char *fileName;
  ConfigType type;
  const char *environmentOverride;
  bool (*loadFunc)(const char *fileName, const shared_ptr<EncFSConfig> &config,
                   ConfigInfo *cfg);
  bool (*saveFunc)(const char *fileName, const shared_ptr<EncFSConfig> &config);
  int currentSubVersion;
  int defaultSubVersion;
} ConfigFileMapping[] = {
      // current format
      {".encfs6.xml", Config_V6, "ENCFS6_CONFIG", readV6Config, writeV6Config,
       V6SubVersion, 0},
      // backward compatible support for older versions
      {".encfs5", Config_V5, "ENCFS5_CONFIG", readV5Config, writeV5Config,
       V5SubVersion, V5SubVersionDefault},
      {".encfs4", Config_V4, NULL, readV4Config, writeV4Config, 0, 0},
      // no longer support earlier versions
      {".encfs3", Config_V3, NULL, NULL, NULL, 0, 0},
      {".encfs2", Config_Prehistoric, NULL, NULL, NULL, 0, 0},
      {".encfs", Config_Prehistoric, NULL, NULL, NULL, 0, 0},
      {NULL, Config_None, NULL, NULL, NULL, 0, 0}};

#include "boost-versioning.h" // IWYU pragma: keep

// define serialization helpers
namespace boost {
namespace serialization {
template <class Archive>
void save(Archive &ar, const EncFSConfig &cfg, unsigned int version) {
  (void)version;
  // version 20 (aka 20100613)
  if (cfg.subVersion == 0)
    ar << make_nvp("version", V6SubVersion);
  else
    ar << make_nvp("version", cfg.subVersion);

  ar << make_nvp("creator", cfg.creator);
  ar << make_nvp("cipherAlg", cfg.cipherIface);
  ar << make_nvp("nameAlg", cfg.nameIface);
  ar << make_nvp("keySize", cfg.keySize);
  ar << make_nvp("blockSize", cfg.blockSize);
  ar << make_nvp("uniqueIV", cfg.uniqueIV);
  ar << make_nvp("chainedNameIV", cfg.chainedNameIV);
  ar << make_nvp("externalIVChaining", cfg.externalIVChaining);
  ar << make_nvp("blockMACBytes", cfg.blockMACBytes);
  ar << make_nvp("blockMACRandBytes", cfg.blockMACRandBytes);
  ar << make_nvp("allowHoles", cfg.allowHoles);

  int encodedSize = cfg.keyData.size();
  ar << make_nvp("encodedKeySize", encodedSize);
  ar << make_nvp("encodedKeyData",
                 serial::make_binary_object(cfg.getKeyData(), encodedSize));

  // version 20080816
  int size = cfg.salt.size();
  ar << make_nvp("saltLen", size);
  ar << make_nvp("saltData",
                 serial::make_binary_object(cfg.getSaltData(), size));
  ar << make_nvp("kdfIterations", cfg.kdfIterations);
  ar << make_nvp("desiredKDFDuration", cfg.desiredKDFDuration);
}

template <class Archive>
void load(Archive &ar, EncFSConfig &cfg, unsigned int version) {
  rInfo("version = %i", version);
  // TODO: figure out how to deprecate all but the first case..
  if (version == 20 || version >= 20100713) {
    rInfo("found new serialization format");
    ar >> make_nvp("version", cfg.subVersion);
  } else if (version == 26800) {
    rInfo("found 20080816 version");
    cfg.subVersion = 20080816;
  } else if (version == 26797) {
    rInfo("found 20080813");
    cfg.subVersion = 20080813;
  } else if (version < (unsigned int)V5SubVersion) {
    rError("Invalid version %i - please fix config file", version);
  } else {
    rInfo("Boost <= 1.41 compatibility mode");
    cfg.subVersion = version;
  }
  rInfo("subVersion = %i", cfg.subVersion);

  ar >> make_nvp("creator", cfg.creator);
  ar >> make_nvp("cipherAlg", cfg.cipherIface);
  ar >> make_nvp("nameAlg", cfg.nameIface);
  ar >> make_nvp("keySize", cfg.keySize);
  ar >> make_nvp("blockSize", cfg.blockSize);
  ar >> make_nvp("uniqueIV", cfg.uniqueIV);
  ar >> make_nvp("chainedNameIV", cfg.chainedNameIV);
  ar >> make_nvp("externalIVChaining", cfg.externalIVChaining);
  ar >> make_nvp("blockMACBytes", cfg.blockMACBytes);
  ar >> make_nvp("blockMACRandBytes", cfg.blockMACRandBytes);
  ar >> make_nvp("allowHoles", cfg.allowHoles);

  int encodedSize;
  ar >> make_nvp("encodedKeySize", encodedSize);
  rAssert(encodedSize == cfg.getCipher()->encodedKeySize());

  unsigned char *key = new unsigned char[encodedSize];
  ar >>
      make_nvp("encodedKeyData", serial::make_binary_object(key, encodedSize));
  cfg.assignKeyData(key, encodedSize);
  delete[] key;

  if (cfg.subVersion >= 20080816) {
    int saltLen;
    ar >> make_nvp("saltLen", saltLen);
    unsigned char *salt = new unsigned char[saltLen];
    ar >> make_nvp("saltData", serial::make_binary_object(salt, saltLen));
    cfg.assignSaltData(salt, saltLen);
    delete[] salt;

    ar >> make_nvp("kdfIterations", cfg.kdfIterations);
    ar >> make_nvp("desiredKDFDuration", cfg.desiredKDFDuration);
  } else {
    cfg.salt.clear();
    cfg.kdfIterations = 16;
    cfg.desiredKDFDuration = NormalKDFDuration;
  }
}

template <class Archive>
void serialize(Archive &ar, EncFSConfig &cfg, unsigned int version) {
  split_free(ar, cfg, version);
}

template <class Archive>
void serialize(Archive &ar, Interface &i, const unsigned int version) {
  (void)version;
  ar &make_nvp("name", i.name());
  ar &make_nvp("major", i.current());
  ar &make_nvp("minor", i.revision());
}
}
}

EncFS_Root::EncFS_Root() {}

EncFS_Root::~EncFS_Root() {}

bool fileExists(const char *fileName) {
  struct stat buf;
  if (!unix::lstat(fileName, &buf)) {
    return true;
  } else {
    // XXX show perror?
    return false;
  }
}

bool isDirectory(const char *fileName) {
  struct stat buf;
  if (!unix::lstat(fileName, &buf)) {
    return S_ISDIR(buf.st_mode);
  } else {
    return false;
  }
}

bool isAbsolutePath(const char *fileName) {
    if (!fileName || fileName[0] == '\0')
	return false;
    if (isalpha((unsigned char) fileName[0]) && fileName[1] == ':')
    return true;
    if (strchr("\\/", fileName[0]) && fileName[1] && strchr("\\/", fileName[1]))
    return true;
    return false;
}

const char *lastPathElement(const char *name) {
  const char *loc = strrchr(name, '/');
  return loc ? loc + 1 : name;
}

std::string parentDirectory(const std::string &path) {
  size_t last = path.find_last_of('/');
  if (last == string::npos)
    return string("");
  else
    return path.substr(0, last);
}

bool userAllowMkdir(const char *path, mode_t mode) {
  return userAllowMkdir(0, path, mode);
}

bool userAllowMkdir(int promptno, const char *path, mode_t mode) {
  // TODO: can we internationalize the y/n names?  Seems strange to prompt in
  // their own language but then have to respond 'y' or 'n'.
  // xgroup(setup)
  cerr << autosprintf(
      _("The directory \"%s\" does not exist. Should it be created? "
        "(y,n) "),
      path);
  char answer[10];
  char *res;

  switch (promptno) {
    case 1:
      cerr << endl << "$PROMPT$ create_root_dir" << endl;
      break;
    case 2:
      cerr << endl << "$PROMPT$ create_mount_point" << endl;
      break;
    default:
      break;
  }
  res = fgets(answer, sizeof(answer), stdin);

  if (res != 0 && toupper(answer[0]) == 'Y') {
    int result = unix::mkdir(path, mode);
    if (result < 0) {
      perror(_("Unable to create directory: "));
      return false;
    } else
      return true;
  } else {
    // Directory not created, by user request
    cerr << _("Directory not created.") << "\n";
    return false;
  }
}

/**
 * Load config file by calling the load function on the filename
 */
ConfigType readConfig_load(ConfigInfo *nm, const char *path,
                           const shared_ptr<EncFSConfig> &config) {
  if (nm->loadFunc) {
    try {
      if ((*nm->loadFunc)(path, config, nm)) {
        config->cfgType = nm->type;
        return nm->type;
      }
	}
	catch (rlog::Error &err) {
		err.log(_RLWarningChannel);
	}

    rError(_("Found config file %s, but failed to load - exiting"), path);
    exit(1);
  } else {
    // No load function - must be an unsupported type..
    config->cfgType = nm->type;
    return nm->type;
  }
}

/**
 * Try to locate the config file
 * Tries the most recent format first, then looks for older versions
 */
ConfigType readConfig(const string &rootDir,
                      const shared_ptr<EncFSConfig> &config) {
  ConfigInfo *nm = ConfigFileMapping;
  while (nm->fileName) {
    // allow environment variable to override default config path
    if (nm->environmentOverride != NULL) {
      char *envFile = getenv(nm->environmentOverride);
      if (envFile != NULL) {
        if (fileExists(envFile)) {
			return readConfig_load(nm, envFile, config);
        }
      }
    }
    // the standard place to look is in the root directory
    string path = rootDir + nm->fileName;
    if (fileExists(path.c_str()))
      return readConfig_load(nm, path.c_str(), config);

    ++nm;
  }

  return Config_None;
}

/**
 * Read config file in current "V6" XML format, normally named ".encfs6.xml"
 * This format is in use since Apr 13, 2008 (commit 6d081f5c)
 */
bool readV6Config(const char *configFile, const shared_ptr<EncFSConfig> &config,
                  ConfigInfo *info) {
  (void)info;

  fs::ifstream st(configFile);
  if (st.is_open()) {
    try {
      boost::archive::xml_iarchive ia(st);
      ia >> BOOST_SERIALIZATION_NVP(*config);

      return true;
    } catch (boost::archive::archive_exception &e) {
      rError("Archive exception: %s", e.what());
      return false;
    }
  } else {
    rInfo("Failed to load config file %s", configFile);
    return false;
  }
}

/**
 * Read config file in deprecated "V5" format, normally named ".encfs5"
 * This format has been used before Apr 13, 2008
 */
bool readV5Config(const char *configFile, const shared_ptr<EncFSConfig> &config,
                  ConfigInfo *info) {
  bool ok = false;

  // use Config to parse the file and query it..
  ConfigReader cfgRdr;
  if (cfgRdr.load(configFile)) {
    try {
      config->subVersion =
          cfgRdr["subVersion"].readInt(info->defaultSubVersion);
      if (config->subVersion > info->currentSubVersion) {
        /* config file specifies a version outside our supported
         range..   */
        rWarning(_("Config subversion %i found, but this version of"
                   " encfs only supports up to version %i."),
                 config->subVersion, info->currentSubVersion);
        return false;
      }
      if (config->subVersion < 20040813) {
        rError(
            _("This version of EncFS doesn't support "
              "filesystems created before 2004-08-13"));
        return false;
      }

      cfgRdr["creator"] >> config->creator;
      cfgRdr["cipher"] >> config->cipherIface;
      cfgRdr["naming"] >> config->nameIface;
      cfgRdr["keySize"] >> config->keySize;
      cfgRdr["blockSize"] >> config->blockSize;

      string data;
      cfgRdr["keyData"] >> data;
      config->assignKeyData(data);
      config->uniqueIV = cfgRdr["uniqueIV"].readBool(false);
      config->chainedNameIV = cfgRdr["chainedIV"].readBool(false);
      config->externalIVChaining = cfgRdr["externalIV"].readBool(false);
      config->blockMACBytes = cfgRdr["blockMACBytes"].readInt(0);
      config->blockMACRandBytes = cfgRdr["blockMACRandBytes"].readInt(0);

      ok = true;
	} catch (rlog::Error &err) {
      err.log(_RLWarningChannel);
      rDebug("Error parsing data in config file %s", configFile);
      ok = false;
    }
  }

  return ok;
}

/**
 * Read config file in deprecated "V4" format, normally named ".encfs4"
 * This format has been used before Jan 7, 2008
 */
bool readV4Config(const char *configFile, const shared_ptr<EncFSConfig> &config,
                  ConfigInfo *info) {
  bool ok = false;

  // use Config to parse the file and query it..
  ConfigReader cfgRdr;
  if (cfgRdr.load(configFile)) {
    try {
      cfgRdr["cipher"] >> config->cipherIface;
      cfgRdr["keySize"] >> config->keySize;
      cfgRdr["blockSize"] >> config->blockSize;
      string data;
      cfgRdr["keyData"] >> data;
      config->assignKeyData(data);

      // fill in default for V4
      config->nameIface = Interface("nameio/stream", 1, 0, 0);
      config->creator = "EncFS 1.0.x";
      config->subVersion = info->defaultSubVersion;
      config->blockMACBytes = 0;
      config->blockMACRandBytes = 0;
      config->uniqueIV = false;
      config->externalIVChaining = false;
      config->chainedNameIV = false;

      ok = true;
	} catch (rlog::Error &err) {
      err.log(_RLWarningChannel);
      rDebug("Error parsing config file %s", configFile);
      ok = false;
    }
  }

  return ok;
}

bool saveConfig(ConfigType type, const string &rootDir,
                const shared_ptr<EncFSConfig> &config) {
  bool ok = false;

  ConfigInfo *nm = ConfigFileMapping;
  while (nm->fileName) {
    if (nm->type == type && nm->saveFunc) {
      string path = rootDir + nm->fileName;
      if (nm->environmentOverride != NULL) {
        // use environment file if specified..
        const char *envFile = getenv(nm->environmentOverride);
        if (envFile != NULL) path.assign(envFile);
      }

      try {
        ok = (*nm->saveFunc)(path.c_str(), config);
      } catch (rlog::Error &err) {
		err.log(_RLWarningChannel);
        ok = false;
      }
      break;
    }
    ++nm;
  }

  return ok;
}

bool writeV6Config(const char *configFile,
                   const shared_ptr<EncFSConfig> &config) {
  fs::ofstream st(configFile);
  if (!st.is_open()) return false;

  st << *config;
  return true;
}

std::ostream &operator<<(std::ostream &st, const EncFSConfig &cfg) {
  boost::archive::xml_oarchive oa(st);
  oa << BOOST_SERIALIZATION_NVP(cfg);

  return st;
}

std::istream &operator>>(std::istream &st, EncFSConfig &cfg) {
  boost::archive::xml_iarchive ia(st);
  ia >> BOOST_SERIALIZATION_NVP(cfg);

  return st;
}

bool writeV5Config(const char *configFile,
                   const shared_ptr<EncFSConfig> &config) {
  ConfigReader cfg;

  cfg["creator"] << config->creator;
  cfg["subVersion"] << config->subVersion;
  cfg["cipher"] << config->cipherIface;
  cfg["naming"] << config->nameIface;
  cfg["keySize"] << config->keySize;
  cfg["blockSize"] << config->blockSize;
  string key;
  key.assign((char *)config->getKeyData(), config->keyData.size());
  cfg["keyData"] << key;
  cfg["blockMACBytes"] << config->blockMACBytes;
  cfg["blockMACRandBytes"] << config->blockMACRandBytes;
  cfg["uniqueIV"] << config->uniqueIV;
  cfg["chainedIV"] << config->chainedNameIV;
  cfg["externalIV"] << config->externalIVChaining;

  return cfg.save(configFile);
}

bool writeV4Config(const char *configFile,
                   const shared_ptr<EncFSConfig> &config) {
  ConfigReader cfg;

  cfg["cipher"] << config->cipherIface;
  cfg["keySize"] << config->keySize;
  cfg["blockSize"] << config->blockSize;
  string key;
  key.assign((char *)config->getKeyData(), config->keyData.size());
  cfg["keyData"] << key;

  return cfg.save(configFile);
}

static Cipher::CipherAlgorithm findCipherAlgorithm(const char *name,
                                                   int keySize) {
  Cipher::AlgorithmList algorithms = Cipher::GetAlgorithmList();
  Cipher::AlgorithmList::const_iterator it;
  for (it = algorithms.begin(); it != algorithms.end(); ++it) {
    if (!strcmp(name, it->name.c_str()) && it->keyLength.allowed(keySize)) {
      return *it;
    }
  }

  Cipher::CipherAlgorithm result;
  return result;
}

/**
 * Ask the user which cipher to use
 */
static Cipher::CipherAlgorithm selectCipherAlgorithm() {
  for (;;) {
    // figure out what cipher they want to use..
    // xgroup(setup)
    cout << _("The following cipher algorithms are available:") << "\n";
    Cipher::AlgorithmList algorithms = Cipher::GetAlgorithmList();
    Cipher::AlgorithmList::const_iterator it;
    int optNum = 1;
    for (it = algorithms.begin(); it != algorithms.end(); ++it, ++optNum) {
      cout << optNum << ". " << it->name << " : "
           << gettext(it->description.c_str()) << "\n";
      if (it->keyLength.min() == it->keyLength.max()) {
        // shown after algorithm name and description.
        // xgroup(setup)
        cout << autosprintf(_(" -- key length %i bits"), it->keyLength.min())
             << "\n";
      } else {
        cout << autosprintf(
                    // shown after algorithm name and description.
                    // xgroup(setup)
                    _(" -- Supports key lengths of %i to %i bits"),
                    it->keyLength.min(), it->keyLength.max()) << "\n";
      }

      if (it->blockSize.min() == it->blockSize.max()) {
        cout << autosprintf(
                    // shown after algorithm name and description.
                    // xgroup(setup)
                    _(" -- block size %i bytes"), it->blockSize.min()) << "\n";
      } else {
        cout << autosprintf(
                    // shown after algorithm name and description.
                    // xgroup(setup)
                    _(" -- Supports block sizes of %i to %i bytes"),
                    it->blockSize.min(), it->blockSize.max()) << "\n";
      }
    }

    // xgroup(setup)
    cout << "\n" << _("Enter the number corresponding to your choice: ");
    char answer[10];
    char *res = fgets(answer, sizeof(answer), stdin);
    int cipherNum = (res == 0 ? 0 : atoi(answer));
    cout << "\n";

    if (cipherNum < 1 || cipherNum > (int)algorithms.size()) {
      cerr << _("Invalid selection.") << "\n";
      continue;
    }

    it = algorithms.begin();
    while (--cipherNum)  // numbering starts at 1
      ++it;

    Cipher::CipherAlgorithm alg = *it;

    // xgroup(setup)
    cout << autosprintf(_("Selected algorithm \"%s\""), alg.name.c_str())
         << "\n\n";

    return alg;
  }
}

/**
 * Ask the user which encoding to use for file names
 */
static Interface selectNameCoding() {
  for (;;) {
    // figure out what cipher they want to use..
    // xgroup(setup)
    cout << _("The following filename encoding algorithms are available:")
         << "\n";
    NameIO::AlgorithmList algorithms = NameIO::GetAlgorithmList();
    NameIO::AlgorithmList::const_iterator it;
    int optNum = 1;
    for (it = algorithms.begin(); it != algorithms.end(); ++it, ++optNum) {
      cout << optNum << ". " << it->name << " : "
           << gettext(it->description.c_str()) << "\n";
    }

    // xgroup(setup)
    cout << "\n" << _("Enter the number corresponding to your choice: ");
    char answer[10];
    char *res = fgets(answer, sizeof(answer), stdin);
    int algNum = (res == 0 ? 0 : atoi(answer));
    cout << "\n";

    if (algNum < 1 || algNum > (int)algorithms.size()) {
      cerr << _("Invalid selection.") << "\n";
      continue;
    }

    it = algorithms.begin();
    while (--algNum)  // numbering starts at 1
      ++it;

    // xgroup(setup)
    cout << autosprintf(_("Selected algorithm \"%s\""), it->name.c_str())
         << "\"\n\n";

    return it->iface;
  }
}

/**
 * Ask the user which key size to use
 */
static int selectKeySize(const Cipher::CipherAlgorithm &alg) {
  if (alg.keyLength.min() == alg.keyLength.max()) {
    cout << autosprintf(_("Using key size of %i bits"), alg.keyLength.min())
         << "\n";
    return alg.keyLength.min();
  }

  cout
      << autosprintf(
             // xgroup(setup)
             _("Please select a key size in bits.  The cipher you have chosen\n"
               "supports sizes from %i to %i bits in increments of %i bits.\n"
               "For example: "),
             alg.keyLength.min(), alg.keyLength.max(), alg.keyLength.inc())
      << "\n";

  int numAvail =
      (alg.keyLength.max() - alg.keyLength.min()) / alg.keyLength.inc();

  if (numAvail < 5) {
    // show them all
    for (int i = 0; i <= numAvail; ++i) {
      if (i) cout << ", ";
      cout << alg.keyLength.min() + i * alg.keyLength.inc();
    }
  } else {
    // partial
    for (int i = 0; i < 3; ++i) {
      if (i) cout << ", ";
      cout << alg.keyLength.min() + i * alg.keyLength.inc();
    }
    cout << " ... " << alg.keyLength.max() - alg.keyLength.inc();
    cout << ", " << alg.keyLength.max();
  }
  // xgroup(setup)
  cout << "\n" << _("Selected key size: ");

  char answer[10];
  char *res = fgets(answer, sizeof(answer), stdin);
  int keySize = (res == 0 ? 0 : atoi(answer));
  cout << "\n";

  keySize = alg.keyLength.closest(keySize);

  // xgroup(setup)
  cout << autosprintf(_("Using key size of %i bits"), keySize) << "\n\n";

  return keySize;
}

/**
 * Ask the user which block size to use
 */
static int selectBlockSize(const Cipher::CipherAlgorithm &alg) {
  if (alg.blockSize.min() == alg.blockSize.max()) {
    cout << autosprintf(
                // xgroup(setup)
                _("Using filesystem block size of %i bytes"),
                alg.blockSize.min()) << "\n";
    return alg.blockSize.min();
  }

  cout << autosprintf(
      // xgroup(setup)
      _("Select a block size in bytes.  The cipher you have chosen\n"
        "supports sizes from %i to %i bytes in increments of %i.\n"
        "Or just hit enter for the default (%i bytes)\n"),
      alg.blockSize.min(), alg.blockSize.max(), alg.blockSize.inc(),
      DefaultBlockSize);

  // xgroup(setup)
  cout << "\n" << _("filesystem block size: ");

  int blockSize = DefaultBlockSize;
  char answer[10];
  char *res = fgets(answer, sizeof(answer), stdin);
  cout << "\n";

  if (res != 0 && atoi(answer) >= alg.blockSize.min()) blockSize = atoi(answer);

  blockSize = alg.blockSize.closest(blockSize);

  // xgroup(setup)
  cout << autosprintf(_("Using filesystem block size of %i bytes"), blockSize)
       << "\n\n";

  return blockSize;
}

/**
 * Prompt the user for a "y" or "n" answer.
 * An empty answer returns defaultValue.
 */
static bool boolDefault(const char *prompt, bool defaultValue) {

  cout << prompt;
  cout << "\n";

  string yesno;

  if (defaultValue == true)
    yesno = "[y]/n: ";
  else
    yesno = "y/[n]: ";

  string response;
  bool value;

  while (true) {
    cout << yesno;
    getline(cin, response);

    if (cin.fail() || response == "") {
      value = defaultValue;
      break;
    } else if (response == "y") {
      value = true;
      break;
    } else if (response == "n") {
      value = false;
      break;
    }
  }

  cout << "\n";
  return value;
}

static bool boolDefaultNo(const char *prompt) {
  return boolDefault(prompt, false);
}

static bool boolDefaultYes(const char *prompt) {
  return boolDefault(prompt, true);
}

/**
 * Ask the user whether to enable block MAC and random header bytes
 */
static void selectBlockMAC(int *macBytes, int *macRandBytes, bool forceMac) {
  bool addMAC = false;
  if (!forceMac) {
    // xgroup(setup)
    addMAC = boolDefaultNo(
        _("Enable block authentication code headers\n"
          "on every block in a file?  This adds about 12 bytes per block\n"
          "to the storage requirements for a file, and significantly affects\n"
          "performance but it also means [almost] any modifications or errors\n"
          "within a block will be caught and will cause a read error."));
  } else {
    cout << "\n\n" << _("You specified --require-macs.  "
                        "Enabling block authentication code headers...")
         << "\n\n";
    addMAC = true;
  }

  if (addMAC)
    *macBytes = 8;
  else
    *macBytes = 0;

  // xgroup(setup)
  cout << _(
      "Add random bytes to each block header?\n"
      "This adds a performance penalty, but ensures that blocks\n"
      "have different authentication codes.  Note that you can\n"
      "have the same benefits by enabling per-file initialization\n"
      "vectors, which does not come with as great of performance\n"
      "penalty. \n"
      "Select a number of bytes, from 0 (no random bytes) to 8: ");

  char answer[10];
  int randSize = 0;
  char *res = fgets(answer, sizeof(answer), stdin);
  cout << "\n";

  randSize = (res == 0 ? 0 : atoi(answer));
  if (randSize < 0) randSize = 0;
  if (randSize > 8) randSize = 8;

  *macRandBytes = randSize;
}

/**
 * Ask the user if per-file unique IVs should be used
 */
static bool selectUniqueIV(bool default_answer) {
  // xgroup(setup)
  return boolDefault(
      _("Enable per-file initialization vectors?\n"
        "This adds about 8 bytes per file to the storage requirements.\n"
        "It should not affect performance except possibly with applications\n"
        "which rely on block-aligned file io for performance."),
      default_answer);
}

/**
 * Ask the user if the filename IV should depend on the complete path
 */
static bool selectChainedIV() {
  // xgroup(setup)
  return boolDefaultYes(
      _("Enable filename initialization vector chaining?\n"
        "This makes filename encoding dependent on the complete path, \n"
        "rather then encoding each path element individually."));
}

/**
 * Ask the user if the file IV should depend on the file path
 */
static bool selectExternalChainedIV() {
  // xgroup(setup)
  return boolDefaultNo(
      _("Enable filename to IV header chaining?\n"
        "This makes file data encoding dependent on the complete file path.\n"
        "If a file is renamed, it will not decode sucessfully unless it\n"
        "was renamed by encfs with the proper key.\n"
        "If this option is enabled, then hard links will not be supported\n"
        "in the filesystem."));
}

/**
 * Ask the user if file holes should be passed through
 */
static bool selectZeroBlockPassThrough() {
  // xgroup(setup)
  return boolDefaultYes(
      _("Enable file-hole pass-through?\n"
        "This avoids writing encrypted blocks when file holes are created."));
}

RootPtr createV6Config(EncFS_Context *ctx, const shared_ptr<EncFS_Opts> &opts) {
  const std::string rootDir = opts->rootDir;
  bool enableIdleTracking = opts->idleTracking;
  bool forceDecode = opts->forceDecode;
  const std::string passwordProgram = opts->passwordProgram;
  bool useStdin = opts->useStdin;
  bool reverseEncryption = opts->reverseEncryption;
  ConfigMode configMode = opts->configMode;
  bool annotate = opts->annotate;

  RootPtr rootInfo;

  // creating new volume key.. should check that is what the user is
  // expecting...
  // xgroup(setup)
  cout << _("Creating new encrypted volume.") << endl;

  char answer[10] = {0};
  if (configMode == Config_Prompt) {
    // xgroup(setup)
    cout << _(
        "Please choose from one of the following options:\n"
        " enter \"x\" for expert configuration mode,\n"
        " enter \"p\" for pre-configured paranoia mode,\n"
        " anything else, or an empty line will select standard mode.\n"
        "?> ");

    if (annotate) cerr << "$PROMPT$ config_option" << endl;

    char *res = fgets(answer, sizeof(answer), stdin);
    //(void)res;
    cout << "\n";
  }

  //                               documented in ...
  int keySize = 0;              // selectKeySize()
  int blockSize = 0;            // selectBlockSize()
  Cipher::CipherAlgorithm alg;  // selectCipherAlgorithm()
  Interface nameIOIface;        // selectNameCoding()
  int blockMACBytes = 0;        // selectBlockMAC()
  int blockMACRandBytes = 0;    // selectBlockMAC()
  bool uniqueIV = true;         // selectUniqueIV()
  bool chainedIV = true;        // selectChainedIV()
  bool externalIV = false;      // selectExternalChainedIV()
  bool allowHoles = true;       // selectZeroBlockPassThrough()
  long desiredKDFDuration = NormalKDFDuration;

  if (reverseEncryption) {
    chainedIV = false;
    externalIV = false;
    uniqueIV = false;
    blockMACBytes = 0;
    blockMACRandBytes = 0;
  }

  if (configMode == Config_Paranoia || answer[0] == 'p') {
    if (reverseEncryption) {
      rError(_("Paranoia configuration not supported for reverse encryption"));
      return rootInfo;
    }

    // xgroup(setup)
    cout << _("Paranoia configuration selected.") << "\n";
    // look for AES with 256 bit key..
    // Use block filename encryption mode.
    // Enable per-block HMAC headers at substantial performance penalty..
    // Enable per-file initialization vector headers.
    // Enable filename initialization vector chaning
    keySize = 256;
    blockSize = DefaultBlockSize;
    alg = findCipherAlgorithm("AES", keySize);
    nameIOIface = BlockNameIO::CurrentInterface();
    blockMACBytes = 8;
    blockMACRandBytes = 0;  // using uniqueIV, so this isn't necessary
    externalIV = true;
    desiredKDFDuration = ParanoiaKDFDuration;
  } else if (configMode == Config_Standard || answer[0] != 'x') {
    // xgroup(setup)
    cout << _("Standard configuration selected.") << "\n";
    // AES w/ 192 bit key, block name encoding, per-file initialization
    // vectors are all standard.
    keySize = 192;
    blockSize = DefaultBlockSize;
    alg = findCipherAlgorithm("AES", keySize);
    nameIOIface = BlockNameIO::CurrentInterface();

    if (opts->requireMac) {
      blockMACBytes = 8;
    }
  }

  if (answer[0] == 'x' || alg.name.empty()) {
    if (answer[0] != 'x') {
      // xgroup(setup)
      cout << _(
          "Sorry, unable to locate cipher for predefined "
          "configuration...\n"
          "Falling through to Manual configuration mode.");
    } else {
      // xgroup(setup)
      cout << _("Manual configuration mode selected.");
    }
    cout << endl;

    // query user for settings..
    alg = selectCipherAlgorithm();
    keySize = selectKeySize(alg);
    blockSize = selectBlockSize(alg);
    nameIOIface = selectNameCoding();
    if (reverseEncryption) {
      cout << _("reverse encryption - chained IV and MAC disabled") << "\n";
      uniqueIV = selectUniqueIV(false);
      /* Reverse mounts are read-only by default (set in main.cpp).
       * If uniqueIV is off, writing can be allowed, because there
       * is no header that could be overwritten */
      if (uniqueIV == false) opts->readOnly = false;
    } else {
      chainedIV = selectChainedIV();
      uniqueIV = selectUniqueIV(true);
      if (chainedIV && uniqueIV)
        externalIV = selectExternalChainedIV();
      else {
        // xgroup(setup)
        cout << _("External chained IV disabled, as both 'IV chaining'\n"
                  "and 'unique IV' features are required for this option.")
             << "\n";
        externalIV = false;
      }
      selectBlockMAC(&blockMACBytes, &blockMACRandBytes, opts->requireMac);
      allowHoles = selectZeroBlockPassThrough();
    }
  }

  shared_ptr<Cipher> cipher = Cipher::New(alg.name, keySize);
  if (!cipher) {
    rError(_("Unable to instanciate cipher %s, key size %i, block size %i"),
           alg.name.c_str(), keySize, blockSize);
    return rootInfo;
  } else {
    rDebug("Using cipher %s, key size %i, block size %i", alg.name.c_str(),
           keySize, blockSize);
  }

  shared_ptr<EncFSConfig> config(new EncFSConfig);

  config->cfgType = Config_V6;
  config->cipherIface = cipher->_interface();
  config->keySize = keySize;
  config->blockSize = blockSize;
  config->nameIface = nameIOIface;
  config->creator = "EncFS " VERSION;
  config->subVersion = V6SubVersion;
  config->blockMACBytes = blockMACBytes;
  config->blockMACRandBytes = blockMACRandBytes;
  config->uniqueIV = uniqueIV;
  config->chainedNameIV = chainedIV;
  config->externalIVChaining = externalIV;
  config->allowHoles = allowHoles;

  config->salt.clear();
  config->kdfIterations = 0;  // filled in by keying function
  config->desiredKDFDuration = desiredKDFDuration;

  cout << "\n";
  // xgroup(setup)
  cout << _("Configuration finished.  The filesystem to be created has\n"
            "the following properties:") << endl;
  showFSInfo(config);

  if (config->externalIVChaining) {
    cout << _("-------------------------- WARNING --------------------------\n")
         << _("The external initialization-vector chaining option has been\n"
              "enabled.  This option disables the use of hard links on the\n"
              "filesystem. Without hard links, some programs may not work.\n"
              "The programs 'mutt' and 'procmail' are known to fail.  For\n"
              "more information, please see the encfs mailing list.\n"
              "If you would like to choose another configuration setting,\n"
              "please press CTRL-C now to abort and start over.") << endl;
    cout << endl;
  }

  // xgroup(setup)
  cout << _(
      "Now you will need to enter a password for your filesystem.\n"
      "You will need to remember this password, as there is absolutely\n"
      "no recovery mechanism.  However, the password can be changed\n"
      "later using encfsctl.\n\n");

  int encodedKeySize = cipher->encodedKeySize();
  unsigned char *encodedKey = new unsigned char[encodedKeySize];

  CipherKey volumeKey = cipher->newRandomKey();

  // get user key and use it to encode volume key
  CipherKey userKey;
  rDebug("useStdin: %i", useStdin);
  if (useStdin) {
    if (annotate) cerr << "$PROMPT$ new_passwd" << endl;
    userKey = config->getUserKey(useStdin);
  } 
  else
    userKey = config->getNewUserKey();

  cipher->writeKey(volumeKey, encodedKey, userKey);
  userKey.reset();

  config->assignKeyData(encodedKey, encodedKeySize);
  delete[] encodedKey;

  if (!volumeKey) {
    rWarning(
        _("Failure generating new volume key! "
          "Please report this error."));
    return rootInfo;
  }

  if (!saveConfig(Config_V6, rootDir, config)) return rootInfo;

  // fill in config struct
  shared_ptr<NameIO> nameCoder =
      NameIO::New(config->nameIface, cipher, volumeKey);
  if (!nameCoder) {
    rWarning(_("Name coding interface not supported"));
    cout << _("The filename encoding interface requested is not available")
         << endl;
    return rootInfo;
  }

  nameCoder->setChainedNameIV(config->chainedNameIV);
  nameCoder->setReverseEncryption(reverseEncryption);

  FSConfigPtr fsConfig(new FSConfig);
  fsConfig->cipher = cipher;
  fsConfig->key = volumeKey;
  fsConfig->nameCoding = nameCoder;
  fsConfig->config = config;
  fsConfig->forceDecode = forceDecode;
  fsConfig->reverseEncryption = reverseEncryption;
  fsConfig->idleTracking = enableIdleTracking;
  fsConfig->opts = opts;

  rootInfo = RootPtr(new EncFS_Root);
  rootInfo->cipher = cipher;
  rootInfo->volumeKey = volumeKey;
  rootInfo->root = shared_ptr<DirNode>(new DirNode(ctx, rootDir, fsConfig));

  return rootInfo;
}

void showFSInfo(const shared_ptr<EncFSConfig> &config) {
  shared_ptr<Cipher> cipher = Cipher::New(config->cipherIface, -1);
  {
    cout << autosprintf(
        // xgroup(diag)
        _("Filesystem cipher: \"%s\", version %i:%i:%i"),
        config->cipherIface.name().c_str(), config->cipherIface.current(),
        config->cipherIface.revision(), config->cipherIface.age());
    // check if we support this interface..
    if (!cipher)
      cout << _(" (NOT supported)\n");
    else {
      // if we're using a newer interface, show the version number
      if (config->cipherIface != cipher->_interface()) {
        Interface iface = cipher->_interface();
        // xgroup(diag)
        cout << autosprintf(_(" (using %i:%i:%i)\n"), iface.current(),
                            iface.revision(), iface.age());
      } else
        cout << "\n";
    }
  }
  {
    // xgroup(diag)
    cout << autosprintf(_("Filename encoding: \"%s\", version %i:%i:%i"),
                        config->nameIface.name().c_str(),
                        config->nameIface.current(),
                        config->nameIface.revision(), config->nameIface.age());

    // check if we support the filename encoding interface..
    shared_ptr<NameIO> nameCoder =
        NameIO::New(config->nameIface, cipher, CipherKey());
    if (!nameCoder) {
      // xgroup(diag)
      cout << _(" (NOT supported)\n");
    } else {
      // if we're using a newer interface, show the version number
      if (config->nameIface != nameCoder->_Interface()) {
        Interface iface = nameCoder->_Interface();
        cout << autosprintf(_(" (using %i:%i:%i)\n"), iface.current(),
                            iface.revision(), iface.age());
      } else
        cout << "\n";
    }
  }
  {
    cout << autosprintf(_("Key Size: %i bits"), config->keySize);
    cipher = config->getCipher();
    if (!cipher) {
      // xgroup(diag)
      cout << _(" (NOT supported)\n");
    } else
      cout << "\n";
  }
  if (config->kdfIterations > 0 && config->salt.size() > 0) {
    cout << autosprintf(_("Using PBKDF2, with %i iterations"),
                        config->kdfIterations) << "\n";
    cout << autosprintf(_("Salt Size: %i bits"), (int)(8 * config->salt.size()))
         << "\n";
  }
  if (config->blockMACBytes || config->blockMACRandBytes) {
    if (config->subVersion < 20040813) {
      cout << autosprintf(
                  // xgroup(diag)
                  _("Block Size: %i bytes + %i byte MAC header"),
                  config->blockSize,
                  config->blockMACBytes + config->blockMACRandBytes) << endl;
    } else {
      // new version stores the header as part of that block size..
      cout << autosprintf(
                  // xgroup(diag)
                  _("Block Size: %i bytes, including %i byte MAC header"),
                  config->blockSize,
                  config->blockMACBytes + config->blockMACRandBytes) << endl;
    }
  } else {
    // xgroup(diag)
    cout << autosprintf(_("Block Size: %i bytes"), config->blockSize);
    cout << "\n";
  }

  if (config->uniqueIV) {
    // xgroup(diag)
    cout << _("Each file contains 8 byte header with unique IV data.\n");
  }
  if (config->chainedNameIV) {
    // xgroup(diag)
    cout << _("Filenames encoded using IV chaining mode.\n");
  }
  if (config->externalIVChaining) {
    // xgroup(diag)
    cout << _("File data IV is chained to filename IV.\n");
  }
  if (config->allowHoles) {
    // xgroup(diag)
    cout << _("File holes passed through to ciphertext.\n");
  }
  cout << "\n";
}

shared_ptr<Cipher> EncFSConfig::getCipher() const {
  return Cipher::New(cipherIface, keySize);
}

void EncFSConfig::assignKeyData(const std::string &in) {
  keyData.assign(in.data(), in.data() + in.length());
}

void EncFSConfig::assignKeyData(unsigned char *data, int len) {
  keyData.assign(data, data + len);
}

void EncFSConfig::assignSaltData(unsigned char *data, int len) {
  salt.assign(data, data + len);
}

unsigned char *EncFSConfig::getKeyData() const {
  return const_cast<unsigned char *>(&keyData.front());
}

unsigned char *EncFSConfig::getSaltData() const {
  return const_cast<unsigned char *>(&salt.front());
}

CipherKey EncFSConfig::makeKey(const char *password, int passwdLen) {
  CipherKey userKey;
  shared_ptr<Cipher> cipher = getCipher();

  // if no salt is set and we're creating a new password for a new
  // FS type, then initialize salt..
  if (salt.size() == 0 && kdfIterations == 0 && cfgType >= Config_V6) {
    // upgrade to using salt
    salt.resize(20);
  }

  if (salt.size() > 0) {
    // if iterations isn't known, then we're creating a new key, so
    // randomize the salt..
    if (kdfIterations == 0 &&
        !cipher->randomize(getSaltData(), salt.size(), true)) {
      cout << _("Error creating salt\n");
      return userKey;
    }

    userKey = cipher->newKey(password, passwdLen, kdfIterations,
                             desiredKDFDuration, getSaltData(), salt.size());
  } else {
    userKey = cipher->newKey(password, passwdLen);
  }

  return userKey;
}

CipherKey EncFSConfig::getUserKey(bool useStdin) {
  char passBuf[MaxPassBuf];
  char *res;

  if (useStdin) {
    res = fgets(passBuf, sizeof(passBuf), stdin);
    // Kill the trailing newline.
    if (passBuf[strlen(passBuf) - 1] == '\n')
      passBuf[strlen(passBuf) - 1] = '\0';
  } else {
    // xgroup(common)
    res = readpassphrase(_("EncFS Password: "), passBuf, sizeof(passBuf),
                         RPP_ECHO_OFF);
  }

  CipherKey userKey;
  if (!res)
    cerr << _("Zero length password not allowed\n");
  else
    userKey = makeKey(passBuf, strlen(passBuf));

  memset(passBuf, 0, sizeof(passBuf));

  return userKey;
}

#if 0
std::string readPassword(int FD) {
  char buffer[1024];
  string result;

  while (1) {
    ssize_t rdSize = recv(FD, buffer, sizeof(buffer), 0);

    if (rdSize > 0) {
      result.append(buffer, rdSize);
      memset(buffer, 0, sizeof(buffer));
    } else
      break;
  }

  // chop off trailing "\n" if present..
  // This is done so that we can use standard programs like ssh-askpass
  // without modification, as it returns trailing newline..
  if (!result.empty() && result[result.length() - 1] == '\n')
    result.resize(result.length() - 1);

  return result;
}

CipherKey EncFSConfig::getUserKey(const std::string &passProg,
                                  const std::string &rootDir) {
  // have a child process run the command and get the result back to us.
  int fds[2], pid;
  int res;
  CipherKey result;

  res = socketpair(PF_UNIX, SOCK_STREAM, 0, fds);
  if (res == -1) {
    perror(_("Internal error: socketpair() failed"));
    return result;
  }
  rDebug("getUserKey: fds = %i, %i", fds[0], fds[1]);

  pid = fork();
  if (pid == -1) {
    perror(_("Internal error: fork() failed"));
    close(fds[0]);
    close(fds[1]);
    return result;
  }

  if (pid == 0) {
    const char *argv[4];
    argv[0] = "/bin/sh";
    argv[1] = "-c";
    argv[2] = passProg.c_str();
    argv[3] = 0;

    // child process.. run the command and send output to fds[0]
    close(fds[1]);  // we don't use the other half..

    // make a copy of stdout and stderr descriptors, and set an environment
    // variable telling where to find them, in case a child wants it..
    int stdOutCopy = dup(STDOUT_FILENO);
    int stdErrCopy = dup(STDERR_FILENO);
    // replace STDOUT with our socket, which we'll used to receive the
    // password..
    dup2(fds[0], STDOUT_FILENO);

    // ensure that STDOUT_FILENO and stdout/stderr are not closed on exec..
    fcntl(STDOUT_FILENO, F_SETFD, 0);  // don't close on exec..
    fcntl(stdOutCopy, F_SETFD, 0);
    fcntl(stdErrCopy, F_SETFD, 0);

    char tmpBuf[8];

    setenv(ENCFS_ENV_ROOTDIR, rootDir.c_str(), 1);

    snprintf(tmpBuf, sizeof(tmpBuf) - 1, "%i", stdOutCopy);
    setenv(ENCFS_ENV_STDOUT, tmpBuf, 1);

    snprintf(tmpBuf, sizeof(tmpBuf) - 1, "%i", stdErrCopy);
    setenv(ENCFS_ENV_STDERR, tmpBuf, 1);

    execvp(argv[0], (char *const *)argv);  // returns only on error..

    perror(_("Internal error: failed to exec program"));
    exit(1);
  }

  close(fds[0]);
  string password = readPassword(fds[1]);
  close(fds[1]);

  waitpid(pid, NULL, 0);

  // convert to key..
  result = makeKey(password.c_str(), password.length());

  // clear buffer..
  password.assign(password.length(), '\0');

  return result;
}
#endif

CipherKey EncFSConfig::getNewUserKey() {
  CipherKey userKey;
  char passBuf[MaxPassBuf];
  char passBuf2[MaxPassBuf];

  do {
    // xgroup(common)
    char *res1 = readpassphrase(_("New Encfs Password: "), passBuf,
                                sizeof(passBuf) - 1, RPP_ECHO_OFF);
    // xgroup(common)
    char *res2 = readpassphrase(_("Verify Encfs Password: "), passBuf2,
                                sizeof(passBuf2) - 1, RPP_ECHO_OFF);

    if (res1 && res2 && !strcmp(passBuf, passBuf2)) {
      userKey = makeKey(passBuf, strlen(passBuf));
    } else {
      // xgroup(common) -- probably not common, but group with the others
      cerr << _("Passwords did not match, please try again\n");
    }

    memset(passBuf, 0, sizeof(passBuf));
    memset(passBuf2, 0, sizeof(passBuf2));
  } while (!userKey);

  return userKey;
}

RootPtr initFS(EncFS_Context *ctx, const shared_ptr<EncFS_Opts> &opts) {
  RootPtr rootInfo;
  shared_ptr<EncFSConfig> config(new EncFSConfig);

  if (readConfig(opts->rootDir, config) != Config_None) {
    if (config->blockMACBytes == 0 && opts->requireMac) {
      cout << _(
          "The configuration disabled MAC, but you passed --require-macs\n");
      return rootInfo;
    }

    if (opts->reverseEncryption) {
      if (config->blockMACBytes != 0 || config->blockMACRandBytes != 0 ||
          config->externalIVChaining || config->chainedNameIV) {
        cout << _(
            "The configuration loaded is not compatible with --reverse\n");
        return rootInfo;
      }
    }

    // first, instanciate the cipher.
    shared_ptr<Cipher> cipher = config->getCipher();
    if (!cipher) {
      rError(_("Unable to find cipher %s, version %i:%i:%i"),
             config->cipherIface.name().c_str(), config->cipherIface.current(),
             config->cipherIface.revision(), config->cipherIface.age());
      // xgroup(diag)
      cout << _("The requested cipher interface is not available\n");
      return rootInfo;
    }

    if (opts->delayMount) {
      rootInfo = RootPtr(new EncFS_Root);
      rootInfo->cipher = cipher;
      rootInfo->root = shared_ptr<DirNode>();
      return rootInfo;
    }

    // get user key
    CipherKey userKey;

    if (opts->passwordProgram.empty()) {
      rDebug("useStdin: %i", opts->useStdin);
      if (opts->annotate) cerr << "$PROMPT$ passwd" << endl;
      userKey = config->getUserKey(opts->useStdin);
    } 

    if (!userKey) return rootInfo;

    rDebug("cipher key size = %i", cipher->encodedKeySize());
    // decode volume key..
    CipherKey volumeKey =
        cipher->readKey(config->getKeyData(), userKey, opts->checkKey);
    userKey.reset();

    if (!volumeKey) {
      // xgroup(diag)
      cout << _("Error decoding volume key, password incorrect\n");
      return rootInfo;
    }

    shared_ptr<NameIO> nameCoder =
        NameIO::New(config->nameIface, cipher, volumeKey);
    if (!nameCoder) {
      rError(_("Unable to find nameio interface %s, version %i:%i:%i"),
             config->nameIface.name().c_str(), config->nameIface.current(),
             config->nameIface.revision(), config->nameIface.age());
      // xgroup(diag)
      cout << _(
          "The requested filename coding interface is "
          "not available\n");
      return rootInfo;
    }

    nameCoder->setChainedNameIV(config->chainedNameIV);
    nameCoder->setReverseEncryption(opts->reverseEncryption);

    FSConfigPtr fsConfig(new FSConfig);
    fsConfig->cipher = cipher;
    fsConfig->key = volumeKey;
    fsConfig->nameCoding = nameCoder;
    fsConfig->config = config;
    fsConfig->forceDecode = opts->forceDecode;
    fsConfig->reverseEncryption = opts->reverseEncryption;
    fsConfig->opts = opts;

    rootInfo = RootPtr(new EncFS_Root);
    rootInfo->cipher = cipher;
    rootInfo->volumeKey = volumeKey;
    rootInfo->root =
        shared_ptr<DirNode>(new DirNode(ctx, opts->rootDir, fsConfig));
  } else {
    if (opts->createIfNotFound) {
      // creating a new encrypted filesystem
      rootInfo = createV6Config(ctx, opts);
    }
  }

  return rootInfo;
}

int remountFS(EncFS_Context *ctx) {
  rDebug("Attempting to reinitialize filesystem");

  RootPtr rootInfo = initFS(ctx, ctx->opts);
  if (rootInfo) {
    ctx->setRoot(rootInfo->root);
    return 0;
  } else {
    rInfo(_("Remount failed"));
    return -EACCES;
  }
}
