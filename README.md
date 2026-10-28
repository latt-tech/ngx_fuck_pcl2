### 关于本项目
为了抑制垃圾PCL2的“1024”超大线程拉取资源而制造的模块。特别简单，资源占用极小。

# ngx_fuck_pcl2

一个 nginx 第三方模块：如果请求的 **User-Agent 里包含 `PCL`（不区分大小写）**，
nginx 直接返回 **444**，也就是一个字节都不回、直接断开连接。

启用方式就是在配置文件里加一行：

```nginx
ngx_fuck_pcl2 on;
```

## 文件

| 文件 | 说明 |
| --- | --- |
| `src/ngx_http_fuck_pcl2_module.c` | 模块源码（单文件，约 350 行） |
| `config` | nginx 的模块描述文件，编译时 `--add-module` 用 |
| `test/nginx.conf` | 示例配置 |
| `test/test.conf`、`test/run-tests.sh` | 功能测试（两个 server：一个开启，一个 `off`） |

## 编译

### 静态编入（推荐）

```sh
./configure --add-module=/path/to/ngx_fuck_pcl2 ...
make && make install
```

### 动态模块

```sh
./configure --with-compat --add-dynamic-module=/path/to/ngx_fuck_pcl2 ...
make modules && make install
```

然后：

```nginx
load_module modules/ngx_http_fuck_pcl2_module.so;
```

`config` 同时支持老式静态 `--add-module` 和新式 `ngx_module_link` 两种写法，
nginx 1.9.11 及以上都可用。

### Windows

本仓库的模块已在 Windows 上实际编译并跑通（nginx 1.28.0 + MSYS2 mingw-w64），
模块本身是标准 nginx C 代码，不依赖平台。Windows 上的 nginx 构建本身有一堆坑
（autoconf 探针上 `test -x`、`int size` 探针在 MSYS 下拿不到输出等），
可以用 MSYS2 的 mingw-w64 工具链配合一个编译器包装脚本绕过，
和模块无关，这里不展开。

## 使用

```nginx
http {
    server {
        listen 80;
        server_name example.com;

        ngx_fuck_pcl2 on;

        location / {
            root /var/www;
        }
    }
}
```

* 指令写在 **`http {}`** 或 **`server {}`** 里，值只能是 `on` / `off`。
* 不写或写 `off` 的 server，模块完全不起作用（直接放行）。
* `location {}` 里不能写这个指令：检查发生在 nginx 选 location 之前，见下文。

## 行为说明

### 匹配规则

* 匹配前先把 User-Agent 的值和关键字都做一次「折叠」，再找子串。
  * ASCII 部分用 `c | 0x20` 折叠，所以 `PCL`、`Pcl`、`pcl`、`pCl` 都命中；
  * 非 ASCII 字节按固定的 Latin-1 规则处理，不受进程 locale 影响。
* 关键字是 `PCL` 和 `PCL/2`。因为 `PCL` 是 `PCL/2` 的前缀，实际生效的是 `PCL`；
  `PCL/2` 只是按需求保留在列表里。
* 只有 **User-Agent 头** 被检查（`r->headers_in.user_agent`），其它头、URL、方法都不看。

示例（`ngx_fuck_pcl2 on;` 时）：

| User-Agent | 结果 |
| --- | --- |
| `PCL/2.11.3` | 444 |
| `PCL2/whatever` | 444 |
| `x/PCL/2` | 444 |
| `Pcl`、`pcl` | 444 |
| `PCLess` | 444（含 `PCL` 就算） |
| `Mozilla/5.0 (...)` | 放行 |
| 没有 User-Agent 头 | 放行 |
| 请求来自 `ngx_fuck_pcl2 off;` 的 server | 放行 |

### 444 是什么

444 是 nginx 的特殊状态码（`NGX_HTTP_CLOSE`）：关闭连接，不发送任何响应字节。
本模块在 `NGX_HTTP_POST_READ_PHASE` 阶段处理，发生在：

1. 读完请求行和请求头之后（不需要等请求体）；
2. 任何 access 阶段、location 匹配、上游转发之前；
3. `access_log` 之前。

所以被拒的请求不会进入 access log（要留痕请看 error log：用 `info` 级别，
每次拦截都会记一条 `ngx_fuck_pcl2: dropping client ...`，User-Agent 只记前 256 字节）。

实测（Windows nginx 1.28.0）：

```
# curl -s -o /dev/null -w '%{http_code}' -A 'PCL/2.11.3' http://127.0.0.1:18081/
000            # curl 退出码 52: Empty reply from server —— 服务器一个字节都没回
# 裸 socket 读: bytes=0
```

### 为什么指令只放在 http / server

检查要在「选 location 之前」做，此时 location 级配置还没有合并、也还拿不到
`r->loc_conf`，所以模块把开关存在 server 级配置里（`ngx_http_get_module_srv_conf`）。
效果是：

* `http{}` 里的 `on` 会被所有 server 继承；
* 单个 server 可以写 `off` 单独关掉；
* `location{}` 里写会被 configure 直接报错拒绝——这是有意的，避免写出
  「看起来关掉了其实没关掉」的配置。

如果你确实需要按 path 精细控制，可以在前面加一层 `if`/`map` 或者干脆把它放在
单独的 server 上。

### 性能

* 只在 server 开启了才做任何工作；
* 每条请求只多一次 `ngx_pnalloc`（大小为 User-Agent 长度）+ 一次 O(n) 扫描，
  关键字 3/5 字节，几乎零成本；
* 关键字列表在配置加载时折叠好，所有 server 共享一份；
* 用户没带 User-Agent 的请求直接放行。

## 安全说明

* 只读请求头的拷贝（通常已经在 nginx 自己的 header 解析里做过拷贝），
  不改客户端缓冲区里的任何字节，也不动 buffer 游标；
* User-Agent 由客户端控制，所以日志里只输出前 256 字节，且不放进引号里
  （nginx 的 `%V` 在引号内遇到长串可打印字符会做转义）。

## 测试

```sh
./test/run-tests.sh /path/to/nginx
```

脚本会在临时目录起一个 nginx（端口 18081/18082），用 curl 发不同 User-Agent，
检查 444/200 以及 access log 里的 444 条数。

本项目实际验证过的环境：nginx 1.28.0 + Windows 10 + MSYS2 mingw-w64，
`srv_conf` 继承、大小写、长 User-Agent（含超过 4K 触发大 header 缓冲的情况）、
HTTP/1.0 无 Host、POST 带 body、200 次并发/半量命中全部符合预期。
