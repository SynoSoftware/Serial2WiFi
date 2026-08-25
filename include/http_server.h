#pragma once

#include "configuration.h"

namespace http_server {

void begin(configuration::ApplyCallback applyConfiguration);
void service();

}  // namespace http_server
