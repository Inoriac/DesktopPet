#pragma once

#include "core/ai/tools/web_content_service.h"
#include <QIODevice>
#include <QStringList>

// Arguments exclude argv[0]. The optional transport is code injection for tests.
int runWebCommand(const QStringList& arguments, QIODevice& input, QIODevice& output,
                  WebHttpGet transport = {});
