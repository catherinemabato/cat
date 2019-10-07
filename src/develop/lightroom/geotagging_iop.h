#pragma once

#include <string>

#include "develop/lightroom/iop.h"

namespace lightroom
{

class GeotaggingIop : public Iop
{
public:
  using Iop::Iop;
  std::string operation_name() const override;
  bool import(xmlDocPtr doc, xmlNodePtr node, const xmlChar *name, const xmlChar *value) override;
  bool apply(int imgid) const override;

private:
  std::string lon_;
  std::string lat_;
};

}