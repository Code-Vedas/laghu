// Copyright Codevedas Inc. 2026-present
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#ifndef LAGHU_RUM_REDIS_MERGE_H
#define LAGHU_RUM_REDIS_MERGE_H

#define LAGHU_RUM_REDIS_MERGE_VERSION 1U
#define LAGHU_RUM_REDIS_BATCH_RECORDS 64U

/*
 * KEYS: batch marker, expiry index, then one record key per delta.
 * ARGV: contract version, TTL, epoch, count, then type/member/delta tuples.
 * The script validates and computes every aggregate before issuing writes.
 */
static const char laghu_rum_redis_merge_script_one[] =
    "local CONTRACT=1\n"
    "local IMAGE_LEN=102\n"
    "local CRITICAL_LEN=1367\n"
    "local INSTRUMENTATION_LEN=7995\n"
    "local function fail(s) return redis.error_reply('LAGHU '..s) end\n"
    "local function keytype(k) local t=redis.call('TYPE',k); if "
    "type(t)=='table' then return t.ok end; return t end\n"
    "local function bytes(s) local b={}; for i=1,#s do b[i]=string.byte(s,i) "
    "end; return b end\n"
    "local function encode(b) local out={}; local unpack_=unpack or "
    "table.unpack; for i=1,#b,256 do local c={}; local n=math.min(i+255,#b); "
    "for j=i,n do c[#c+1]=b[j] end; out[#out+1]=string.char(unpack_(c)) end; "
    "return table.concat(out) end\n"
    "local function eq(a,b,p,n) for i=p,p+n-1 do if a[i]~=b[i] then return "
    "false end end; return true end\n"
    "local function u32(a,p) return "
    "a[p]+a[p+1]*256+a[p+2]*65536+a[p+3]*16777216 end\n"
    "local function maxle(a,b,p,n) for i=p+n-1,p,-1 do if b[i]>a[i] then for "
    "j=p,p+n-1 do a[j]=b[j] end; return elseif b[i]<a[i] then return end end "
    "end\n"
    "local function satadd(a,b,p,n) local v={}; local carry=0; for i=0,n-1 do "
    "local x=a[p+i]+b[p+i]+carry; v[i+1]=x%256; carry=math.floor(x/256) end; "
    "if carry~=0 then for i=0,n-1 do a[p+i]=255 end else for i=0,n-1 do "
    "a[p+i]=v[i+1] end end end\n"
    "local function merge(kind,current,delta)\n"
    " if kind==4 then if current and current~=delta then return nil,'decision "
    "identity' end; return current or delta end\n"
    " local expected=kind==3 and IMAGE_LEN or (kind==2 and CRITICAL_LEN or "
    "(kind==1 and INSTRUMENTATION_LEN or 0))\n"
    " if expected==0 or #delta~=expected or (current and #current~=expected) "
    "then return nil,'record length' end\n"
    " local d=bytes(delta); if u32(d,1)~=1 then return nil,'record version' "
    "end\n"
    " if not current then if kind==3 and d[102]>1 then return nil,'image flag' "
    "end; return delta end\n"
    " local a=bytes(current); if u32(a,1)~=1 then return nil,'stored version' "
    "end\n"
    " if kind==3 then\n"
    "  if not eq(a,d,5,65) or d[102]>1 or a[102]>1 then return nil,'image "
    "identity' end\n"
    "  maxle(a,d,70,8); for p=78,98,4 do maxle(a,d,p,4) end; if d[102]~=0 then "
    "a[102]=1 end\n"
    " elseif kind==2 then\n"
    "  if not eq(a,d,5,65) or not eq(a,d,1094,65) or not eq(a,d,1159,65) then "
    "return nil,'critical identity' end\n"
    "  maxle(a,d,1224,8); satadd(a,d,1232,4); satadd(a,d,1236,2); "
    "satadd(a,d,1238,2); for p=1240,1367 do a[p]=bit.bor(a[p],d[p]) end\n"
    " else\n"
    "  if not eq(a,d,5,195) or not eq(a,d,208,4164) then return "
    "nil,'instrumentation identity' end\n"
    "  local scripts=u32(a,208); if scripts>64 then return nil,'script count' "
    "end\n"
    "  maxle(a,d,200,8)\n"
    "  for bucket=0,1 do local base=4372+bucket*1812; satadd(a,d,base,8); for "
    "j=0,4 do satadd(a,d,base+8+j*8,8); maxle(a,d,base+48+j*4,4) end; for "
    "j=0,23 do satadd(a,d,base+68+j*8,8) end; satadd(a,d,base+260,8); "
    "satadd(a,d,base+268,8); for j=0,scripts-1 do satadd(a,d,base+276+j*8,8); "
    "satadd(a,d,base+788+j*8,8); satadd(a,d,base+1300+j*8,8) end end\n"
    " end\n"
    " return encode(a)\n"
    "end\n";

static const char laghu_rum_redis_merge_script_two[] =
    "local version=tonumber(ARGV[1]); local ttl=tonumber(ARGV[2]); local "
    "now=tonumber(ARGV[3]); local count=tonumber(ARGV[4])\n"
    "if version~=CONTRACT or not ttl or ttl<1 or not now or now<0 or not count "
    "or count<0 or count>64 or #KEYS~=count+2 or #ARGV~=4+count*3 then return "
    "fail('contract') end\n"
    "if keytype(KEYS[1])~='none' and keytype(KEYS[1])~='string' then return "
    "fail('batch type') end\n"
    "if keytype(KEYS[2])~='none' and keytype(KEYS[2])~='zset' then return "
    "fail('index type') end\n"
    "local duplicate=redis.call('EXISTS',KEYS[1])==1; local merged={}; local "
    "seen={}\n"
    "for i=1,count do local rk=KEYS[i+2]; if seen[rk] then return "
    "fail('duplicate key') end; seen[rk]=true; local kt=keytype(rk); if "
    "kt~='none' and kt~='string' then return fail('record type') end; local "
    "kind=tonumber(ARGV[5+(i-1)*3]); local member=ARGV[6+(i-1)*3]; local "
    "delta=ARGV[7+(i-1)*3]; local old=redis.call('GET',rk); if duplicate then "
    "if not old then return fail('duplicate expired') end; merged[i]=old else "
    "local value,err=merge(kind,old,delta); if not value then return fail(err) "
    "end; merged[i]=value end end\n"
    "if not duplicate then for i=1,count do local rk=KEYS[i+2]; local "
    "member=ARGV[6+(i-1)*3]; redis.call('SET',rk,merged[i],'EX',ttl); "
    "redis.call('ZADD',KEYS[2],now+ttl,member) end; "
    "redis.call('EXPIRE',KEYS[2],ttl); "
    "redis.call('SET',KEYS[1],tostring(CONTRACT),'EX',ttl) end\n"
    "local result={duplicate and 1 or 0}; for i=1,count do "
    "result[#result+1]=merged[i] end; return result\n";

#endif
