#include "../Helpers/RulesHelper.h"

#include "../ESPEasyCore/ESPEasy_Log.h"
#include "../Globals/Settings.h"
#include "../Helpers/ESPEasy_Storage.h"


/********************************************************************************************\
   Test for common mistake
   Return true if mistake was found (and corrected)
 \*********************************************************************************************/
bool rules_replace_common_mistakes(const String& from, const String& to, String& line)
{
  if (line.indexOf(from) == -1) {
    return false; // Nothing replaced
  }

  if (loglevelActiveFor(LOG_LEVEL_ERROR)) {
    String log;

    if (log.reserve(32 + from.length() + to.length() + line.length())) {
      log  = F("Rules (Syntax Error, auto-corrected): '");
      log += from;
      log += F("' => '");
      log += to;
      log += F("' in: '");
      log += line;
      log += '\'';
      addLogMove(LOG_LEVEL_ERROR, log);
    }
  }
  line.replace(from, to);
  return true;
}

/********************************************************************************************\
   Check for common mistakes
   Return true if nothing strange found
 \*********************************************************************************************/
bool check_rules_line_user_errors(String& line)
{
  bool res = true;

  if (rules_replace_common_mistakes(F("if["), F("if ["), line)) {
    res = false;
  }

  if (rules_replace_common_mistakes(F("if%"), F("if %"), line)) {
    res = false;
  }

  return res;
}

/********************************************************************************************\
   Strip comment from the line.
   Return true when comment was stripped.
 \*********************************************************************************************/
bool rules_strip_trailing_comments(String& line)
{
  // Strip trailing comments
  int comment = line.indexOf(F("//"));

  if (comment >= 0) {
    line = line.substring(0, comment);
    line.trim();
    return true;
  }
  return false;
}

RulesHelperClass::RulesHelperClass()
{}

RulesHelperClass::~RulesHelperClass()
{
  closeAllFiles();
}

#ifdef ESP32

void RulesHelperClass::closeAllFiles() {
  for (auto it = _fileHandleMap.begin(); it != _fileHandleMap.end();) {
    it = _fileHandleMap.erase(it);
  }
}

#else // ifdef ESP32

void RulesHelperClass::closeAllFiles() {
  for (auto it = _fileHandleMap.begin(); it != _fileHandleMap.end();) {
    it->second.close();
    it = _fileHandleMap.erase(it);
  }
}

size_t RulesHelperClass::read(const String& filename, size_t& pos, uint8_t *buffer, size_t length)
{
  if (!Settings.UseRules || !fileExists(filename)) {
    return 0;
  }
  auto it = _fileHandleMap.find(filename);

  if (it == _fileHandleMap.end()) {
    // No open file handle found, so try to open it.
    _fileHandleMap.emplace(filename, tryOpenFile(filename, "r+"));
    it = _fileHandleMap.find(filename);
  }

  if (!it->second) {
    return 0;
  }

  if (it->second.position() != pos) {
    it->second.seek(pos);
  }
  const size_t ret = it->second.read(buffer, length);

  pos = it->second.position();
  return ret;
}

#endif // ifdef ESP32

bool RulesHelperClass::addChar(char c, String& line,   bool& firstNonSpaceRead,  bool& commentFound)
{
  switch (c)
  {
    case '\n':
    {
      // Line end, parse rule
      line.trim();

      if ((line.length() > 0) && !line.startsWith(F("//"))) {
        rules_strip_trailing_comments(line);
        check_rules_line_user_errors(line);
        return true;
      }

      // Prepare for new line
      line.clear();
      firstNonSpaceRead = false;
      commentFound      = false;
      break;
    }
    case '\r': // Just skip this character
      break;
    case '\t': // tab
    case ' ':  // space
    {
      // Strip leading spaces.
      if (firstNonSpaceRead) {
        line += ' ';
      }
      break;
    }
    case '/':
    {
      if (!commentFound) {
        line += '/';

        if (line.endsWith(F("//"))) {
          // consider the rest of the line a comment
          commentFound = true;
        }
      }
      break;
    }
    default: // Any other character
    {
      firstNonSpaceRead = true;

      if (!commentFound) {
        line += c;
      }
      break;
    }
  }
  return false;
}

#ifdef ESP32
String RulesHelperClass::readLn(const String& filename,
                                size_t      & pos,
                                bool        & moreAvailable,
                                bool          searchNextOnBlock)
{
  auto it = _fileHandleMap.find(filename);

  if (it == _fileHandleMap.end()) {
    // Read lines from the file
    fs::File f = tryOpenFile(filename, "r+");

    if (f) {
      RulesLines lines;
      String     tmpStr;
      bool firstNonSpaceRead = false;
      bool commentFound      = false;

      while (f.available()) {
        if (addChar(char(f.read()), tmpStr, firstNonSpaceRead, commentFound)) {
          lines.push_back(tmpStr);
          firstNonSpaceRead = false;
          commentFound      = false;
          tmpStr.clear();
        }
      }

      if (tmpStr.length() > 0) {
        rules_strip_trailing_comments(tmpStr);
        check_rules_line_user_errors(tmpStr);
        lines.push_back(tmpStr);
        tmpStr.clear();
      }

      if (loglevelActiveFor(LOG_LEVEL_INFO)) {
        String log = F("Rules : Read ");
        log += lines.size();
        log += F(" lines from ");
        log += filename;
        addLogMove(LOG_LEVEL_INFO, log);
      }
      _fileHandleMap.emplace(std::make_pair(filename, std::move(lines)));
      it = _fileHandleMap.find(filename);
    }
  }

  if (it != _fileHandleMap.end()) {
    while (pos < it->second.size()) {
      ++pos;
      moreAvailable = pos < it->second.size();

      if (!searchNextOnBlock ||
          it->second[pos - 1].substring(0, 3).equalsIgnoreCase(F("on "))) {
        return it->second[pos - 1];
      }
    }
  }
  return EMPTY_STRING;
}

#else // ifdef ESP32

String RulesHelperClass::readLn(const String& filename,
                                size_t      & pos,
                                bool        & moreAvailable,
                                bool          searchNextOnBlock)
{
  std::vector<uint8_t> buf;

  buf.resize(RULES_BUFFER_SIZE);

  bool firstNonSpaceRead = false;
  bool commentFound      = false;

  // Try to get the best possible estimate on line length based on earlier parsing of the rules.
  static size_t longestLineSize = RULES_BUFFER_SIZE;
  String line;

  line.reserve(longestLineSize);

  bool done = false;

  while (!done) {
    const size_t startPos = pos;
    int len               = read(filename, pos, &buf[0], RULES_BUFFER_SIZE);
    moreAvailable = len != 0;

    if (!moreAvailable) { done = true; }

    for (int x = 0; x < len; x++) {
      int data = buf[x];

      if (addChar(char(data), line, firstNonSpaceRead, commentFound)) {
        if (line.length() > longestLineSize) {
          longestLineSize = line.length();
        }

        // A line may end on every position in the buffer,
        // so we must make sure the position is reflecting the end of the line.
        pos = startPos + x;

        if (!searchNextOnBlock ||
            line.substring(0, 3).equalsIgnoreCase(F("on ")))
        {
          done = true;

          return line;
        } else {
          // Not starting with "on " which we need, so continue to search for a matching line
          line.clear();
        }
      }
    }
  }
  rules_strip_trailing_comments(line);
  check_rules_line_user_errors(line);
  return line;
}

#endif // ifdef ESP32
