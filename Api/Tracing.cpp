/**
 * @file Tracing.cpp
 * Implements class Tracing.
 */

#include "Debug/Log.hpp"
#include "Debug/Tracer.hpp"

#include "Tracing.hpp"

namespace Api
{

unsigned Tracing::s_traceStackDepth = 0;

void Tracing::enableTrace(std::string traceName, unsigned depth)
{
  CALL("Tracing::enableTrace");
  ENABLE_TAG_LIMITED(traceName.c_str(), depth);
}

void Tracing::processTraceString(std::string str)
{
  CALL("Tracing::processTraceString");
  PROCESS_TRACE_SPEC_STRING(str);
}

void Tracing::pushTracingState()
{
  CALL("Tracing::pushTracingState");

  PUSH_TAG_STATES();
  s_traceStackDepth++;
}

void Tracing::popTracingState()
{
  CALL("Tracing::popTracingState");

  if(s_traceStackDepth==0) {
    throw "No pushed tracing state left to be popped";
  }
  s_traceStackDepth--;
  POP_TAG_STATES();
}

void Tracing::displayHelp()
{
  CALL("Tracing::displayHelp");
  DISPLAY_HELP();
}

}
