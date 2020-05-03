// Copyright 2019-2020 The Lithe Project Development Team
//
// Please read attached license file for information

/* converts vector to string
 *
 * vector<Y::Z> x = theOutputOfThis(0, 10);
 * ostringstream oss;
 * copy(x.begin(), x.end(), CommaIterator(oss, ","));
 * string result = oss.str();
 * const char *x_result = result.c_str();
 *
 */

#include <iterator>
#include <vector>
#include <iostream>
#include <sstream>

struct CommaIterator:
public std::iterator<std::output_iterator_tag, void, void, void, void> {
  std::ostream *os;
  std::string comma;
  bool first;
  
  CommaIterator(std::ostream& os, const std::string& comma) :
    os(&os), comma(comma), first(true) {}

  CommaIterator& operator++() { return *this; }
  CommaIterator& operator++(int) { return *this; }
  CommaIterator& operator*() { return *this; }
  template <class T>
  CommaIterator& operator=(const T& t) {
    if(first)
      first = false;
    else
      *os << comma;
    *os << t;
    return *this;
  }
};
  