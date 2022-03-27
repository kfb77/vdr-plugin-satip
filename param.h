/*
 * param.h: SAT>IP plugin for the Video Disk Recorder
 *
 * See the README file for copyright information and how to reach the author.
 *
 */

#pragma once

#include <string>

std::string GetTransponderUrlParameters(const cChannel* channel);
std::string GetTnrUrlParameters(const cChannel* channel);
