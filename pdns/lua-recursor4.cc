/*
 * This file is part of PowerDNS or dnsdist.
 * Copyright -- PowerDNS.COM B.V. and its contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * In addition, for the avoidance of any doubt, permission is granted to
 * link this program with OpenSSL and to (re)distribute the binaries
 * produced as the result of such linking.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include "lua-recursor4.hh"
#include <fstream>
#include "logger.hh"
#include "dnsparser.hh"
#include "syncres.hh"
#include "namespaces.hh"
#include "rec_channel.hh"
#include "ednsoptions.hh"
#include "ednssubnet.hh"
#include "filterpo.hh"
#include "rec-snmp.hh"
#include <unordered_set>

RecursorLua4::RecursorLua4(const std::string &fname) : BaseLua4(fname) {
}

static int followCNAMERecords(vector<DNSRecord>& ret, const QType& qtype)
{
  vector<DNSRecord> resolved;
  DNSName target;
  for(const DNSRecord& rr :  ret) {
    if(rr.d_type == QType::CNAME) {
      auto rec = getRR<CNAMERecordContent>(rr);
      if(rec) {
        target=rec->getTarget();
        break;
      }
    }
  }
  if(target.empty())
    return 0;
  
  int rcode=directResolve(target, qtype, 1, resolved); // 1 == class
  
  for(const DNSRecord& rr :  resolved) {
    ret.push_back(rr);
  }
  return rcode;
 
}

static int getFakeAAAARecords(const DNSName& qname, const std::string& prefix, vector<DNSRecord>& ret)
{
  int rcode=directResolve(qname, QType(QType::A), 1, ret);

  ComboAddress prefixAddress(prefix);

  for(DNSRecord& rr :  ret)
  {
    if(rr.d_type == QType::A && rr.d_place==DNSResourceRecord::ANSWER) {
      if(auto rec = getRR<ARecordContent>(rr)) {
        ComboAddress ipv4(rec->getCA());
        uint32_t tmp;
        memcpy((void*)&tmp, &ipv4.sin4.sin_addr.s_addr, 4);
        // tmp=htonl(tmp);
        memcpy(((char*)&prefixAddress.sin6.sin6_addr.s6_addr)+12, &tmp, 4);
        rr.d_content = std::make_shared<AAAARecordContent>(prefixAddress);
        rr.d_type = QType::AAAA;
      }
    }
  }
  return rcode;
}

static int getFakePTRRecords(const DNSName& qname, const std::string& prefix, vector<DNSRecord>& ret)
{
  /* qname has a reverse ordered IPv6 address, need to extract the underlying IPv4 address from it
     and turn it into an IPv4 in-addr.arpa query */
  ret.clear();
  vector<string> parts = qname.getRawLabels();

  if(parts.size() < 8)
    return -1;

  string newquery;
  for(int n = 0; n < 4; ++n) {
    newquery +=
      std::to_string(stoll(parts[n*2], 0, 16) + 16*stoll(parts[n*2+1], 0, 16));
    newquery.append(1,'.');
  }
  newquery += "in-addr.arpa.";


  int rcode = directResolve(DNSName(newquery), QType(QType::PTR), 1, ret);
  for(DNSRecord& rr :  ret)
  {
    if(rr.d_type == QType::PTR && rr.d_place==DNSResourceRecord::ANSWER) {
      rr.d_name = qname;
    }
  }
  return rcode;

}

boost::optional<dnsheader> RecursorLua4::DNSQuestion::getDH() const
{
  if (dh)
    return *dh;
  return boost::optional<dnsheader>();
}

vector<string> RecursorLua4::DNSQuestion::getEDNSFlags() const
{
  vector<string> ret;
  if (ednsFlags) {
    if (*ednsFlags & EDNSOpts::DNSSECOK)
      ret.push_back("DO");
  }
  return ret;
}

bool RecursorLua4::DNSQuestion::getEDNSFlag(string flag) const
{
  if (ednsFlags) {
    if (flag == "DO" && (*ednsFlags & EDNSOpts::DNSSECOK))
      return true;
  }
  return false;
}

vector<pair<uint16_t, string> > RecursorLua4::DNSQuestion::getEDNSOptions() const
{
  if(ednsOptions)
    return *ednsOptions;
  else
    return vector<pair<uint16_t,string>>();
}

boost::optional<string>  RecursorLua4::DNSQuestion::getEDNSOption(uint16_t code) const
{
  if(ednsOptions)
    for(const auto& o : *ednsOptions)
      if(o.first==code)
        return o.second;
        
  return boost::optional<string>();
}

boost::optional<Netmask>  RecursorLua4::DNSQuestion::getEDNSSubnet() const
{

  if(ednsOptions) {
    for(const auto& o : *ednsOptions) {
      if(o.first==EDNSOptionCode::ECS) {
        EDNSSubnetOpts eso;
        if(getEDNSSubnetOptsFromString(o.second, &eso))
          return eso.source;
        else 
          break;
      }
    }
  }
  return boost::optional<Netmask>();
}


vector<pair<int, DNSRecord> > RecursorLua4::DNSQuestion::getRecords() const
{
  vector<pair<int, DNSRecord> > ret;
  int num=1;
  for(const auto& r : records) {
    ret.push_back({num++, r});
  }
  return ret;
}
void RecursorLua4::DNSQuestion::setRecords(const vector<pair<int, DNSRecord> >& recs)
{
  records.clear();
  for(const auto& p : recs) {
    records.push_back(p.second);
  }
}

void RecursorLua4::DNSQuestion::addRecord(uint16_t type, const std::string& content, DNSResourceRecord::Place place, boost::optional<int> ttl, boost::optional<string> name)
{
  DNSRecord dr;
  dr.d_name=name ? DNSName(*name) : qname;
  dr.d_ttl=ttl.get_value_or(3600);
  dr.d_type = type;
  dr.d_place = place;
  dr.d_content = DNSRecordContent::mastermake(type, 1, content);
  records.push_back(dr);
}

void RecursorLua4::DNSQuestion::addAnswer(uint16_t type, const std::string& content, boost::optional<int> ttl, boost::optional<string> name)
{
  addRecord(type, content, DNSResourceRecord::ANSWER, ttl, name);
}

struct DynMetric
{
  std::atomic<unsigned long>* ptr;
  void inc() { (*ptr)++; }
  void incBy(unsigned int by) { (*ptr)+= by; }
  unsigned long get() { return *ptr; }
  void set(unsigned long val) { *ptr =val; }
};

void RecursorLua4::postPrepareContext()
{
  d_lw->registerMember<const DNSName (DNSQuestion::*)>("qname", [](const DNSQuestion& dq) -> const DNSName& { return dq.qname; }, [](DNSQuestion& dq, const DNSName& newName) { (void) newName; });
  d_lw->registerMember<uint16_t (DNSQuestion::*)>("qtype", [](const DNSQuestion& dq) -> uint16_t { return dq.qtype; }, [](DNSQuestion& dq, uint16_t newType) { (void) newType; });
  d_lw->registerMember<bool (DNSQuestion::*)>("isTcp", [](const DNSQuestion& dq) -> bool { return dq.isTcp; }, [](DNSQuestion& dq, bool newTcp) { (void) newTcp; });
  d_lw->registerMember<const ComboAddress (DNSQuestion::*)>("localaddr", [](const DNSQuestion& dq) -> const ComboAddress& { return dq.local; }, [](DNSQuestion& dq, const ComboAddress& newLocal) { (void) newLocal; });
  d_lw->registerMember<const ComboAddress (DNSQuestion::*)>("remoteaddr", [](const DNSQuestion& dq) -> const ComboAddress& { return dq.remote; }, [](DNSQuestion& dq, const ComboAddress& newRemote) { (void) newRemote; });
  d_lw->registerMember<vState (DNSQuestion::*)>("validationState", [](const DNSQuestion& dq) -> vState { return dq.validationState; }, [](DNSQuestion& dq, vState newState) { (void) newState; });

  d_lw->registerMember<bool (DNSQuestion::*)>("variable", [](const DNSQuestion& dq) -> bool { return dq.variable; }, [](DNSQuestion& dq, bool newVariable) { dq.variable = newVariable; });
  d_lw->registerMember<bool (DNSQuestion::*)>("wantsRPZ", [](const DNSQuestion& dq) -> bool { return dq.wantsRPZ; }, [](DNSQuestion& dq, bool newWantsRPZ) { dq.wantsRPZ = newWantsRPZ; });

  d_lw->registerMember("rcode", &DNSQuestion::rcode);
  d_lw->registerMember("tag", &DNSQuestion::tag);
  d_lw->registerMember("requestorId", &DNSQuestion::requestorId);
  d_lw->registerMember("followupFunction", &DNSQuestion::followupFunction);
  d_lw->registerMember("followupPrefix", &DNSQuestion::followupPrefix);
  d_lw->registerMember("followupName", &DNSQuestion::followupName);
  d_lw->registerMember("data", &DNSQuestion::data);
  d_lw->registerMember("udpQuery", &DNSQuestion::udpQuery);
  d_lw->registerMember("udpAnswer", &DNSQuestion::udpAnswer);
  d_lw->registerMember("udpQueryDest", &DNSQuestion::udpQueryDest);
  d_lw->registerMember("udpCallback", &DNSQuestion::udpCallback);
  d_lw->registerMember("appliedPolicy", &DNSQuestion::appliedPolicy);
  d_lw->registerMember<DNSFilterEngine::Policy, std::string>("policyName",
    [](const DNSFilterEngine::Policy& pol) -> std::string {
      if(pol.d_name)
        return *pol.d_name;
      return std::string();
    },
    [](DNSFilterEngine::Policy& pol, const std::string& name) {
      pol.d_name = std::make_shared<std::string>(name);
    });
  d_lw->registerMember("policyKind", &DNSFilterEngine::Policy::d_kind);
  d_lw->registerMember("policyTTL", &DNSFilterEngine::Policy::d_ttl);
  d_lw->registerMember<DNSFilterEngine::Policy, std::string>("policyCustom",
    [](const DNSFilterEngine::Policy& pol) -> std::string {
      if(pol.d_custom)
        return pol.d_custom->getZoneRepresentation();
      return std::string();
    },
    [](DNSFilterEngine::Policy& pol, const std::string& content) {
      // Only CNAMES for now, when we ever add a d_custom_type, there will be pain
      pol.d_custom = DNSRecordContent::mastermake(QType::CNAME, 1, content);
    }
  );
  d_lw->registerFunction("getDH", &DNSQuestion::getDH);
  d_lw->registerFunction("getEDNSOptions", &DNSQuestion::getEDNSOptions);
  d_lw->registerFunction("getEDNSOption", &DNSQuestion::getEDNSOption);
  d_lw->registerFunction("getEDNSSubnet", &DNSQuestion::getEDNSSubnet);
  d_lw->registerFunction("getEDNSFlags", &DNSQuestion::getEDNSFlags);
  d_lw->registerFunction("getEDNSFlag", &DNSQuestion::getEDNSFlag);
  d_lw->registerMember("name", &DNSRecord::d_name);
  d_lw->registerMember("type", &DNSRecord::d_type);
  d_lw->registerMember("ttl", &DNSRecord::d_ttl);
  d_lw->registerMember("place", &DNSRecord::d_place);

  d_lw->registerMember("size", &EDNSOptionView::size);
  d_lw->registerFunction<std::string(EDNSOptionView::*)()>("getContent", [](const EDNSOptionView& option) { return std::string(option.content, option.size); });

  d_lw->registerFunction<string(DNSRecord::*)()>("getContent", [](const DNSRecord& dr) { return dr.d_content->getZoneRepresentation(); });
  d_lw->registerFunction<boost::optional<ComboAddress>(DNSRecord::*)()>("getCA", [](const DNSRecord& dr) { 
      boost::optional<ComboAddress> ret;

      if(auto rec = std::dynamic_pointer_cast<ARecordContent>(dr.d_content))
        ret=rec->getCA(53);
      else if(auto aaaarec = std::dynamic_pointer_cast<AAAARecordContent>(dr.d_content))
        ret=aaaarec->getCA(53);
      return ret;
    });


  d_lw->registerFunction<void(DNSRecord::*)(const std::string&)>("changeContent", [](DNSRecord& dr, const std::string& newContent) { dr.d_content = DNSRecordContent::mastermake(dr.d_type, 1, newContent); });
  d_lw->registerFunction("addAnswer", &DNSQuestion::addAnswer);
  d_lw->registerFunction("addRecord", &DNSQuestion::addRecord);
  d_lw->registerFunction("getRecords", &DNSQuestion::getRecords);
  d_lw->registerFunction("setRecords", &DNSQuestion::setRecords);

  d_lw->registerFunction<void(DNSQuestion::*)(const std::string&)>("addPolicyTag", [](DNSQuestion& dq, const std::string& tag) { if (dq.policyTags) { dq.policyTags->push_back(tag); } });
  d_lw->registerFunction<void(DNSQuestion::*)(const std::vector<std::pair<int, std::string> >&)>("setPolicyTags", [](DNSQuestion& dq, const std::vector<std::pair<int, std::string> >& tags) {
      if (dq.policyTags) {
        dq.policyTags->clear();
        for (const auto& tag : tags) {
          dq.policyTags->push_back(tag.second);
        }
      }
    });
  d_lw->registerFunction<std::vector<std::pair<int, std::string> >(DNSQuestion::*)()>("getPolicyTags", [](const DNSQuestion& dq) {
      std::vector<std::pair<int, std::string> > ret;
      if (dq.policyTags) {
        int count = 1;
        for (const auto& tag : *dq.policyTags) {
          ret.push_back({count++, tag});
        }
      }
      return ret;
    });

  d_lw->registerFunction<void(DNSQuestion::*)(const std::string&)>("discardPolicy", [](DNSQuestion& dq, const std::string& policy) {
      if (dq.discardedPolicies) {
        (*dq.discardedPolicies)[policy] = true;
      }
    });

  d_lw->writeFunction("newDS", []() { return SuffixMatchNode(); });
  d_lw->registerFunction<void(SuffixMatchNode::*)(boost::variant<string,DNSName, vector<pair<unsigned int,string> > >)>(
    "add",
    [](SuffixMatchNode&smn, const boost::variant<string,DNSName,vector<pair<unsigned int,string> > >& in){
      try {
        if(auto s = boost::get<string>(&in)) {
          smn.add(DNSName(*s));
        }
        else if(auto v = boost::get<vector<pair<unsigned int, string> > >(&in)) {
          for(const auto& entry : *v)
            smn.add(DNSName(entry.second));
        }
        else {
          smn.add(boost::get<DNSName>(in));
        }
      }
      catch(std::exception& e) {
        theL() <<Logger::Error<<e.what()<<endl;
      }
    }
  );

  d_lw->registerFunction("check",(bool (SuffixMatchNode::*)(const DNSName&) const) &SuffixMatchNode::check);
  d_lw->registerFunction("toString",(string (SuffixMatchNode::*)() const) &SuffixMatchNode::toString);

  d_pd.push_back({"policykinds", in_t {
    {"NoAction", (int)DNSFilterEngine::PolicyKind::NoAction},
    {"Drop",     (int)DNSFilterEngine::PolicyKind::Drop    },
    {"NXDOMAIN", (int)DNSFilterEngine::PolicyKind::NXDOMAIN},
    {"NODATA",   (int)DNSFilterEngine::PolicyKind::NODATA  },
    {"Truncate", (int)DNSFilterEngine::PolicyKind::Truncate},
    {"Custom",   (int)DNSFilterEngine::PolicyKind::Custom  }
    }});

  for(const auto& n : QType::names)
    d_pd.push_back({n.first, n.second});

  d_pd.push_back({"validationstates", in_t{
        {"Indeterminate", Indeterminate },
        {"Bogus", Bogus },
        {"Insecure", Insecure },
        {"Secure", Secure },
  }});

  d_pd.push_back({"now", &g_now});

  d_lw->writeFunction("getMetric", [](const std::string& str) {
      return DynMetric{getDynMetric(str)};
    });

  d_lw->registerFunction("inc", &DynMetric::inc);
  d_lw->registerFunction("incBy", &DynMetric::incBy);
  d_lw->registerFunction("set", &DynMetric::set);
  d_lw->registerFunction("get", &DynMetric::get);

  d_lw->writeFunction("getStat", [](const std::string& str) {
      uint64_t result = 0;
      optional<uint64_t> value = getStatByName(str);
      if (value) {
        result = *value;
      }
      return result;
    });

  d_lw->writeFunction("getRecursorThreadId", []() {
      return getRecursorThreadId();
    });

  d_lw->writeFunction("sendCustomSNMPTrap", [](const std::string& str) {
      if (g_snmpAgent) {
        g_snmpAgent->sendCustomTrap(str);
      }
    });
}

void RecursorLua4::postLoad() {
  d_prerpz = d_lw->readVariable<boost::optional<luacall_t>>("prerpz").get_value_or(0);
  d_preresolve = d_lw->readVariable<boost::optional<luacall_t>>("preresolve").get_value_or(0);
  d_nodata = d_lw->readVariable<boost::optional<luacall_t>>("nodata").get_value_or(0);
  d_nxdomain = d_lw->readVariable<boost::optional<luacall_t>>("nxdomain").get_value_or(0);
  d_postresolve = d_lw->readVariable<boost::optional<luacall_t>>("postresolve").get_value_or(0);
  d_preoutquery = d_lw->readVariable<boost::optional<luacall_t>>("preoutquery").get_value_or(0);

  d_ipfilter = d_lw->readVariable<boost::optional<ipfilter_t>>("ipfilter").get_value_or(0);
  d_gettag = d_lw->readVariable<boost::optional<gettag_t>>("gettag").get_value_or(0);
}

bool RecursorLua4::prerpz(DNSQuestion& dq, int& ret)
{
  return genhook(d_prerpz, dq, ret);
}

bool RecursorLua4::preresolve(DNSQuestion& dq, int& ret)
{
  return genhook(d_preresolve, dq, ret);
}

bool RecursorLua4::nxdomain(DNSQuestion& dq, int& ret)
{
  return genhook(d_nxdomain, dq, ret);
}

bool RecursorLua4::nodata(DNSQuestion& dq, int& ret)
{
  return genhook(d_nodata, dq, ret);
}

bool RecursorLua4::postresolve(DNSQuestion& dq, int& ret)
{
  return genhook(d_postresolve, dq, ret);
}

bool RecursorLua4::preoutquery(const ComboAddress& ns, const ComboAddress& requestor, const DNSName& query, const QType& qtype, bool isTcp, vector<DNSRecord>& res, int& ret)
{
  bool variableAnswer = false;
  bool wantsRPZ = false;
  RecursorLua4::DNSQuestion dq(ns, requestor, query, qtype.getCode(), isTcp, variableAnswer, wantsRPZ);
  dq.currentRecords = &res;

  return genhook(d_preoutquery, dq, ret);
}

bool RecursorLua4::ipfilter(const ComboAddress& remote, const ComboAddress& local, const struct dnsheader& dh)
{
  if(d_ipfilter)
    return d_ipfilter(remote, local, dh);
  return false; // don't block
}

unsigned int RecursorLua4::gettag(const ComboAddress& remote, const Netmask& ednssubnet, const ComboAddress& local, const DNSName& qname, uint16_t qtype, std::vector<std::string>* policyTags, LuaContext::LuaObject& data, const std::map<uint16_t, EDNSOptionView>& ednsOptions, bool tcp, std::string& requestorId, std::string& deviceId)
{
  if(d_gettag) {
    auto ret = d_gettag(remote, ednssubnet, local, qname, qtype, ednsOptions, tcp);

    if (policyTags) {
      const auto& tags = std::get<1>(ret);
      if (tags) {
        for (const auto& tag : *tags) {
          policyTags->push_back(tag.second);
        }
      }
    }
    const auto dataret = std::get<2>(ret);
    if (dataret) {
      data = *dataret;
    }
    const auto reqIdret = std::get<3>(ret);
    if (reqIdret) {
      requestorId = *reqIdret;
    }
    const auto deviceIdret = std::get<4>(ret);
    if (deviceIdret) {
      deviceId = *deviceIdret;
    }
    return std::get<0>(ret);
  }
  return 0;
}

bool RecursorLua4::genhook(luacall_t& func, DNSQuestion& dq, int& ret)
{
  if(!func)
    return false;

  if (dq.currentRecords) {
    dq.records = *dq.currentRecords;
  } else {
    dq.records.clear();
  }

  dq.followupFunction.clear();
  dq.followupPrefix.clear();
  dq.followupName.clear();
  dq.udpQuery.clear();
  dq.udpAnswer.clear();
  dq.udpCallback.clear();

  dq.rcode = ret;
  bool handled=func(&dq);

  if(handled) {
loop:;
    ret=dq.rcode;
    
    if(!dq.followupFunction.empty()) {
      if(dq.followupFunction=="followCNAMERecords") {
        ret = followCNAMERecords(dq.records, QType(dq.qtype));
      }
      else if(dq.followupFunction=="getFakeAAAARecords") {
        ret=getFakeAAAARecords(dq.followupName, dq.followupPrefix, dq.records);
      }
      else if(dq.followupFunction=="getFakePTRRecords") {
        ret=getFakePTRRecords(dq.followupName, dq.followupPrefix, dq.records);
      }
      else if(dq.followupFunction=="udpQueryResponse") {
        dq.udpAnswer = GenUDPQueryResponse(dq.udpQueryDest, dq.udpQuery);
        auto cbFunc = d_lw->readVariable<boost::optional<luacall_t>>(dq.udpCallback).get_value_or(0);
        if(!cbFunc) {
          theL()<<Logger::Error<<"Attempted callback for Lua UDP Query/Response which could not be found"<<endl;
          return false;
        }
        bool result=cbFunc(&dq);
        if(!result) {
          return false;
        }
        goto loop;
      }
    }
    if (dq.currentRecords) {
      *dq.currentRecords = dq.records;
    }
  }

  // see if they added followup work for us too
  return handled;
}

RecursorLua4::~RecursorLua4(){}
