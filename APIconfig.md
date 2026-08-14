# 语音听写（流式版）WebAPI 文档
文档地址：https://www.xfyun.cn/doc/asr/voicedictation/API.html#%E6%8E%A5%E5%8F%A3demo

## 一、接口说明
语音听写流式接口，用于1分钟内的即时语音转文字技术，支持实时返回识别结果，实现边上传音频边获取识别文本。

### 高阶功能-动态修正（免费开放）
1. **未开启动态修正**：实时追加返回识别结果，不会覆盖历史内容
2. **开启动态修正**：返回结果可追加，也可替换修正历史片段，结果颗粒度更细
    - 开通方式：控制台-流式听写-高级功能开启
    - 限制：仅支持中文
    - 开启/未开启返回JSON格式不同，见「动态修正返回结果」章节

### 小语种能力
- 支持语种：控制台「语音听写」页面/控制台查看
- 调用区分：小语种与中英文使用不同WebSocket域名，见「接口要求」
- 参数配置：业务参数章节查看语种配置方式

### 协议优势
本接口基于WebSocket API提供流式能力，适用边说边识别场景：
1. 对比SDK：轻量、跨语言，无需集成本地SDK
2. 对比HTTP同步接口：WebSocket原生支持跨域、实时流式推送
3. 旧版说明：原HTTP普通版接口`http[s]: //api.xfyun.cn/v1/service/v1/iat`不再对外开放，存量用户可继续使用，建议迁移流式WebSocket接口

## 二、接口Demo
示例Demo可点击官方链接下载，目前提供Java/Python/JS/Go/NodeJS等语言示例，其他语言可参照本文档自行开发；也可到讯飞开放平台社区分享自定义Demo。

## 三、接口要求
| 内容 | 说明 |
|------|------|
| 请求协议 | ws[s]，强烈推荐wss加密协议 |
| 请求地址 | 中英文通用：`wss://iat-api.xfyun.cn/v2/iat`、`wss://ws-api.xfyun.cn/v2/iat`<br>小语种专用：`wss://iat-niche-api.xfyun.cn/v2/iat`<br>⚠️ 禁止写死服务器IP，固定使用域名调用 |
| 请求行 | `GET /v2/iat HTTP/1.1` |
| 鉴权方式 | HMAC-SHA256签名机制，见「接口鉴权」 |
| 字符编码 | UTF-8 |
| 响应格式 | 统一JSON |
| 开发语言 | 任意可发起WebSocket请求的编程语言 |
| 操作系统 | 无限制 |
| 音频硬性属性 | 采样率16k/8K、位深16bit、单声道 |
| 支持音频格式 | pcm、speex(8k)、speex-wb(16k)、mp3<br>mp3仅中文普通话/英文可用，方言、小语种暂不支持 |
| 音频时长限制 | 单条最长60秒 |
| 语种支持 | 中文、英文、小语种、中文方言；控制台添加试用/购买对应语种权限 |

## 四、接口调用流程
1. 基于APIKey、APISecret使用HMAC-SHA256生成签名，拼接鉴权URL发起WebSocket握手
2. 握手成功后，双向通信：客户端分片上传音频、服务端实时推送识别文本
3. 音频全部上传完成，客户端发送`status=2`结束帧标识会话结束
4. 收到服务端完整结果结束标识后，主动关闭WebSocket连接

### WebSocket使用注意事项
1. 服务端仅支持websocket-version 13，客户端框架需兼容该版本
2. 服务端所有返回帧均为TextMessage（opcode=1），解析异常请升级/更换客户端框架
3. JSON分包导致解析失败：客户端WebSocket协议解析存在缺陷，更换框架即可
4. 正常关闭连接建议使用错误码1000，框架不支持则无需处理

## 五、IP白名单
1. 默认关闭IP白名单，不限调用外网IP
2. 开启后服务端校验请求外网IP，不在白名单则拒绝服务
3. 配置规则：控制台对应服务页面编辑，保存后约5分钟生效；每个Appid每个服务需独立配置；仅填写**外网公网IP**，局域网IP无效
4. 报错标识：握手返回`{"message":"Your IP address is not allowed"}`，代表白名单配置错误/未生效

## 六、接口鉴权
握手阶段URL拼接鉴权参数，服务端校验签名合法性。

### 完整鉴权URL示例
```
wss://iat-api.xfyun.cn/v2/iat?authorization=YXBpX2tleT0ia2V5eHh4eHh4eHg4ZWUyNzkzNDg1MTlleHh4eHh4eHgiLCBhbGdvcml0aG09ImhtYWMtc2hhMjU2IiwgaGVhZGVycz0iaG9zdCBkYXRlIHJlcXVlc3QtbGluZSIsIHNpZ25hdHVyZT0iSHAzVHk0WmtTQm1MOGpLeU9McFFpdjlTcjVudm1lWUVIN1dzTC9aTzJKZz0i&date=Wed%2C%2010%20Jul%202019%2007%3A35%3A43%20GMT&host=iat-api.xfyun.cn
```

### 鉴权参数说明
| 参数 | 类型 | 必填 | 说明 |
|------|------|------|------|
| host | string | 是 | 请求主机域名，如iat-api.xfyun.cn |
| date | string | 是 | UTC/GMT时区，RFC1123时间格式，示例`Wed, 10 Jul 2019 07:35:43 GMT`<br>服务端允许最大300秒时钟偏差，超出直接拒绝请求 |
| authorization | string | 是 | Base64编码后的完整签名信息，生成规则见下文 |

### authorization 参数生成完整步骤
1. **获取密钥**：控制台创建WebAPI应用并开通语音听写服务，获取32位`APIKey`、`APISecret`
2. **拼接authorization原始字符串（authorization_origin）**
    ```
    api_key="$api_key",algorithm="hmac-sha256",headers="host date request-line",signature="$signature"
    ```
    - algorithm固定`hmac-sha256`
    - headers固定字符串`host date request-line`（仅参数名，非参数值）
3. **拼接签名原始串 signature_origin**（换行分隔，`:`后带空格）
    ```
    host: $host
    date: $date
    GET $request-path HTTP/1.1
    ```
    示例：
    ```
    host: iat-api.xfyun.cn
    date: Wed, 10 Jul 2019 07:35:43 GMT
    GET /v2/iat HTTP/1.1
    ```
4. **生成SHA256签名摘要**
    `signature_sha = HmacSHA256(signature_origin, APISecret)`
5. **Base64编码摘要得到signature**
    `signature = Base64Encode(signature_sha)`
6. **填充到authorization_origin后，整体Base64编码**
    `authorization = Base64Encode(authorization_origin)`

### Golang 鉴权URL拼接示例代码
```go
//@hosturl :  like  wss://iat-api.xfyun.cn/v2/iat
//@apikey : apiKey
//@apiSecret : apiSecret
func assembleAuthUrl(hosturl string, apiKey, apiSecret string) string {
    ul, err := url.Parse(hosturl)
    if err != nil {
        fmt.Println(err)
    }
    //签名时间
    date := time.Now().UTC().Format(time.RFC1123)
    //参与签名的字段 host ,date, request-line
    signString := []string{"host: " + ul.Host, "date: " + date, "GET " + ul.Path + " HTTP/1.1"}
    //拼接签名字符串
    sgin := strings.Join(signString, "\n")
    //签名结果
    sha := HmacWithShaTobase64("hmac-sha256", sgin, apiSecret)
    //构建请求参数 此时不需要urlencoding
    authUrl := fmt.Sprintf("api_key=\"%s\", algorithm=\"%s\", headers=\"%s\", signature=\"%s\"", apiKey,
        "hmac-sha256", "host date request-line", sha)
    //将请求参数使用base64编码
    authorization:= base64.StdEncoding.EncodeToString([]byte(authUrl))
    v := url.Values{}
    v.Add("host", ul.Host)
    v.Add("date", date)
    v.Add("authorization", authorization)
    //将编码后的字符串url encode后添加到url后面
    callurl := hosturl + "?" + v.Encode()
    return callurl
}
```

### 握手鉴权失败错误码与处理
| HTTP Code | 错误信息 | 问题原因 & 解决方案 |
|-----------|----------|--------------------|
| 401 | `Unauthorized` | URL缺失authorization参数，核对鉴权拼接逻辑 |
| 401 | `HMAC signature cannot be verified` | 签名参数解析失败，检查APIKey复制、拼接格式 |
| 401 | `HMAC signature does not match` | 签名校验不匹配：<br>1. APIKey/APISecret复制错误<br>2. host/date/request-line拼接格式错误<br>3. signature Base64长度异常（正常44字符） |
| 403 | 时钟偏移校验失败 | 本地服务器时间偏差超过5分钟，同步标准UTC时间 |
| 403 | `Your IP address is not allowed` | IP白名单未关闭/配置外网IP错误，等待5分钟生效 |

握手失败响应示例：
```http
HTTP/1.1 401 Forbidden
Date: Thu, 06 Dec 2018 07:55:16 GMT
Content-Length: 116
Content-Type: text/plain; charset=utf-8
{
    "message": "HMAC signature does not match"
}
```

## 七、接口数据传输与接收
1. 音频分片发送建议：间隔40ms发送一帧，帧字节为标准大小整数倍，发送间隔过短易识别错乱
2. 单会话限制：最长60秒；连续10秒未上传音频，服务端主动断开连接
3. 帧大小标准建议
    - PCM原始音频：单帧1280字节
    - 讯飞定制speex 16k 7级压缩：61字节/帧整数倍
    - 标准开源speex 16k 7级压缩：60字节/帧整数倍
4. speex压缩等级对应帧长参考（文档内置表格）

### 请求数据结构说明
所有上行数据为JSON字符串，分三大块：
1. `common`：公共参数，**仅握手后第一帧上传一次**
2. `business`：业务识别参数，**仅握手后第一帧上传一次**
3. `data`：音频流参数，**所有帧必须携带**

#### 1. common 公共参数
| 参数名 | 类型 | 必填 | 描述 |
|--------|------|------|------|
| app_id | string | 是 | 讯飞平台应用APPID |

#### 2. business 业务参数
| 参数名 | 类型 | 必填 | 取值&说明 |
|--------|------|------|-----------|
| language | string | 是 | `zh_cn`中文；`en_us`英文；小语种查看控制台 |
| domain | string | 是 | `iat`日常通用；medical医疗；gov-seat-assistant政务坐席；seat-assistant金融坐席；gov-ansys政务语音分析；gov-nav政务导航；fin-nav金融导航；fin-ansys金融质检<br>垂直领域需单独授权，未授权报11200 |
| accent | string | 是 | 中文场景生效：`mandarin`普通话；其他方言控制台开通 |
| vad_eos | int | 否 | 静默断句超时(ms)，默认2000；小语种默认关闭VAD |
| dwa | string | 否 | 仅中文；填`wpgs`开启动态修正，控制台免费开通 |
| pd | string | 否 | 领域个性化热词：game游戏/health健康/shopping购物/trip旅行，需授权 |
| ptt | int | 否 | 标点开关：1开启(默认)、0关闭 |
| rlang | string | 否 | 输出字体：`zh-cn`简体(默认)、`zh-hk`繁体香港，免费开通 |
| vinfo | int | 否 | 时间戳返回开关：0关闭(默认)、1开启；开启后不可同时使用dwa=wpgs |
| nunum | int | 否 | 数字阿拉伯化：1开启(默认)、0关闭；支持中文普通话/日语 |
| speex_size | int | 否 | speex标识：1=标准开源speex；2=讯飞定制speex（无需填） |
| nbest | int | 否 | 句子多候选，取值1~5；开启增加200ms延迟，控制台免费开通 |
| wbest | int | 否 | 词语多候选，取值1~5；开启增加200ms延迟，控制台免费开通 |

#### 3. data 音频流参数（每帧必传）
| 参数名 | 类型 | 必填 | 说明 |
|--------|------|------|------|
| status | int | 是 | 0=首帧音频；1=中间分片；2=结束帧（音频传完必须发一帧status=2） |
| format | string | 是 | 16k：`audio/L16;rate=16000`；8k：`audio/L16;rate=8000` |
| encoding | string | 是 | raw=PCM原始；speex=8k压缩；speex-wb=16k压缩；lame=mp3 |
| audio | string | 是 | 音频二进制Base64编码字符串 |

### 首帧完整请求示例
```json
{
    "common":{
        "app_id":"123456"
    },
    "business":{
        "language":"zh_cn",
        "domain":"iat",
        "accent":"mandarin"
    },
    "data":{
        "status":0,
        "format":"audio/L16;rate=16000",
        "encoding":"raw",
        "audio":"exSI6ICJlbiIsCgkgICAgInBvc2l0aW9uIjogImZhbHNlIgoJf..."
    }
}
```

### 音频结束帧（仅标识结束，无音频数据）
```json
{
    "data":{
        "status":2
    }
}
```

## 八、服务端返回参数说明
基础返回字段（所有响应通用）
| 参数 | 类型 | 描述 |
|------|------|------|
| sid | string | 会话唯一ID，用于问题排查，首帧结果返回 |
| code | int | 返回码，0=成功，非0为异常 |
| message | string | 错误/成功描述文本 |
| data | object | 识别结果主体 |
| data.status | int | 结果分片标识：0首块、1中间块、2最终完整结果 |
| data.result | object | 听写文本结果 |
| data.result.sn | int | 本次返回分片序号 |
| data.result.ls | bool | 是否本轮会话最后一片文本 |
| data.result.ws | array | 分词结果数组 |
| data.result.ws.bg | int | 词语起始帧偏移（1帧=10ms）；标点/超长结果bg=0无效 |
| data.result.ws.cw | array | 单字/词语候选数组 |
| data.result.ws.cw.w | string | 识别文字 |
| sc/wb/wc/we/wp | int/string | 保留字段，无需解析 |

### 扩展返回字段（功能开启后生效）
#### 1. 动态修正 dwa=wpgs（仅中文，与vinfo互斥）
| 字段 | 取值 | 说明 |
|------|------|------|
| data.result.pgs | apd / rpl | apd=追加文本；rpl=替换历史分片 |
| data.result.rg | [start,end] | rpl模式生效，替换sn序号区间 |

动态修正返回示例：
```json
{
  "code": 0,
  "message": "success",
  "sid": "iatxxxxxxxxxxxxx",
  "data": {
    "result": {
      "bg": 0,
      "ed": 0,
      "ls": false,
      "pgs": "rpl",
      "rg": [1,1],
      "sn": 2,
      "ws": [
        {"bg":0,"cw":[{"sc":0,"w":"测试"}]},
        {"bg":0,"cw":[{"sc":0,"w":"一下"}]}
      ]
    },
    "status": 1
  }
}
```

#### 2. vinfo=1 时间戳（不可与wpgs同时开启）
```json
{
  "code": 0,
  "message": "success",
  "sid": "iatxxxxxxxxxxxxxx",
  "data": {
    "result": {
      "bg": 0,
      "ed": 0,
      "ls": false,
      "sn": 1,
      "vad": {
        "ws": [{"bg": 40,"ed": 366,"eg": 63.58}]
      },
      "ws": [{"bg":53,"cw":[{"sc":0,"w":"4月"}]}]
    },
    "status": 1
  }
}
```

#### 3. nbest 句子多候选示例
```json
{
  "code": 0,
  "message": "success",
  "sid": "iatxxxxxxxxxxxxx",
  "data": {
    "result": {
      "bg": 0,
      "ed": 0,
      "ls": false,
      "sn": 1,
      "ws": [
        {
          "bg": 35,
          "cw": [
            {"sc": 0,"w": "打电话给梁玉生"},
            {"sc": 0,"w": "打电话给梁玉升"}
          ]
        }
      ]
    },
    "status": 0
  }
}
```

#### 4. wbest 词语多候选示例
```json
{
  "code": 0,
  "message": "success",
  "sid": "iatxxxxxxxxxxxxxx",
  "data": {
    "result": {
      "bg": 0,
      "ed": 0,
      "ls": false,
      "sn": 1,
      "ws": [
        {"bg": 159,"cw": [{"sc":0,"w":"梁"}]},
        {
          "bg": 191,
          "cw": [{"sc":0,"w":"玉"},{"sc":0,"w":"育"}]
        },
        {
          "bg": 215,
          "cw": [{"sc":0,"w":"生"},{"sc":0,"w":"升"}]
        }
      ]
    },
    "status": 0
  }
}
```

## 九、错误码对照表
| 错误码 | 描述 | 处理方案 |
|--------|------|----------|
| 10005 | licc fail appid授权失败 | APPID错误，或未开通听写服务 |
| 10006 | Get audio rate fail | 请求参数缺失/格式错误，核对报错参数 |
| 10007 | get invalid rate | 参数取值超出合法范围 |
| 10010 | AIGES_ERROR_NO_LICENSE | 引擎权限耗尽，提交工单扩容 |
| 10014 / 10019 / 10114 | session timeout 会话超时 | 音频超过60秒；或10s未发数据；发送完音频未发status=2结束帧 |
| 10043 | Audio解码失败 | speex音频帧大小不匹配、编码格式错误 |
| 10101 | 引擎会话已结束 | 服务端已结束识别，客户端仍持续发送音频 |
| 10139 | invalid param 参数错误 | JSON结构、音频编码参数非法 |
| 10313 | appid不能为空 | 首帧common未携带app_id字段 |
| 10317 | invalid version | 接口版本异常，联系讯飞技术支持 |
| 11200 | auth no license 无权限 | 使用未开通功能、语种/垂直领域未购买授权 |
| 11201 | 日流控超限 | 联系商务提升每日并发额度 |
| 10160 | JSON解析失败 | 上行数据非标准JSON |
| 10161 | Base64解码失败 | audio字段编码错误、包含非法字符 |
| 10163 | 参数校验失败 | 缺失必填参数；frameSize Base64后超过13000字节（建议固定1280字节） |
| 10165 | 无效句柄 | 首帧音频未设置status=0 |
| 10200 | 读取数据超时 | 连续10秒未上传音频，服务端主动断开 |

## 十、调用Demo资源
> 注：官方Demo仅基础示例，不建议直接上线生产环境
1. Java 语音听写流式Demo
2. Python3 语音听写流式Demo
3. JavaScript 浏览器端Demo
4. Golang Demo
5. NodeJS Demo
6. JS SDK Github开源地址

其他语言可参照本文接口规范自行开发，代码可分享至讯飞开放社区。

## 十一、音频样例资源
官方提供各规格测试音频，用于调试格式：
1. 中文普通话 16k PCM
2. 中文普通话 8k PCM
3. 16k MP3
4. 8k MP3
5. 8k 7级标准开源speex
6. 16k 7级标准开源speex
7. 16k 7级讯飞定制speex
8. 8k 7级讯飞定制speex

音频格式校验工具推荐：Cool Edit Pro；speex编码工具见官方配套文档。

## 十二、配套视频教程
语音听写WebAPI完整接口详解视频

## 十三、常见问题FAQ
### Q1：APIKey在哪里获取？
A：控制台 → 我的应用 → 对应应用 → 语音听写（流式版）服务页面查看APIKey/APISecret。

### Q2：识别结果为空、不全、文字错乱？
1. 音频格式不达标：必须16bit、单声道、8k/16k采样；mp3仅中英文可用
2. 静音片段过长：默认2000ms自动截断识别，调整vad_eos最大10000ms

### Q3：支持哪些音频格式？
PCM(raw)、speex、speex-wb、mp3；mp3仅限中文普通话、英文。音频硬性标准：16bit、单声道、8/16kHz采样。

### Q4：单次最多识别多久音频？
单会话上限60秒。

### Q5：默认并发多少路？如何扩容？
默认50路并发；需要更高并发提交工单联系技术人员扩容。

### Q6：报错10163 length of $.data.audio must be between 0,13000
单帧音频Base64编码后字节上限13000，推荐固定每帧原始PCM 1280字节，不要增大分片。

### Q7：长时间不发音频自动断开？
vad_eos静默超时触发断开；同时连续10秒无音频上传服务端主动关闭连接。

### Q8：热词支持多少个？能否扩容？
控制台单应用最多2000条热词，暂不支持扩容。

### Q9：如何限制仅指定IP调用接口？
控制台开启IP白名单，填写服务外网公网IP，保存5分钟后生效，不在列表IP直接拒绝握手。