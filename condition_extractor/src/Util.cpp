#include "Util.hpp"
#include "md5/md5.h"
#include <fstream>

std::string computeHash(std::string file_path) {
  md5::MD5 md5stream;

  std::ifstream a_file;
  a_file.open(file_path);

  // get length of file
  a_file.seekg(0, std::ios::end);
  size_t length = a_file.tellg();
  a_file.seekg(0, std::ios::beg);

  char *buffer = (char *)malloc(length);

  // read file
  a_file.read(buffer, length);
  md5stream.add(buffer, length);

  free(buffer);
  buffer = NULL;

  a_file.close();
  return md5stream.getHash();
}
