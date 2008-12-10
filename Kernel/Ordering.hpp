/**
 * @file Ordering.hpp
 * Defines (abstract) class Ordering for simplification orderings
 *
 * @since 30/04/2008 flight Brussels-Tel Aviv
 */

#ifndef __Ordering__
#define __Ordering__

#include "../Forwards.hpp"

namespace Kernel {

/**
 * An abstract class for simplification orderings
 * @since 30/04/2008 flight Brussels-Tel Aviv
 */
class Ordering
{
public:
  /** Represents the results of ordering comparisons */
  enum Result {
    GREATER,
    LESS,
    GREATER_EQ,
    LESS_EQ,
    EQUAL,
    INCOMPARABLE
  };
  virtual ~Ordering() {}
  /** Return the result of comparing @b l1 and @b l2 */
  virtual Result compare(Literal* l1,Literal* l2) = 0;
  /** Return the result of comparing terms (not term lists!)
   * @b t1 and @b t2 */
  virtual Result compare(TermList* t1,TermList* t2) = 0;
}; // class Ordering

}

#endif
