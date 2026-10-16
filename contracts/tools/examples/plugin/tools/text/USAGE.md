# 文本计数示例

这是作者填写声明的示例。实际使用前，作者提供脚本，使用者配置 Python 与脚本路径；当前目录不安装解释器、不接入主体。

## 适用场景与输入

使用 `example.text/count` 统计已取得文本的 Unicode 码点数量。text 来自使用者输入或其他操作返回的文本；本操作不接受文件路径，不负责提取文件内容。

它不统计单词、字节或用户感知的字符簇。例如组合字符的计数可能大于屏幕上看到的字符数量。

## 准备与调用

配置 python 为 Python 3 的绝对路径，scriptPath 为作者提供脚本的绝对路径。程序通过 JSON stdin 接收完整参数，不依赖当前工作目录。

参数 `{"text":"你好abc"}` 的正常结果为 `{"count":5}`。退出码为 0、输出符合结构时，表示本次计数已经完成，不存在仅受理状态。

## 失败和影响

错误输入、脚本不可执行或解释器缺失时说明错误原因，不把空输出解释为计数 0。该参考实现只处理传入文本，不修改外部状态。

## 作者脚本参考

下列代码用于解释实现必须满足的输入输出约定，不是项目运行时实现：

```python
import json
import sys

try:
    args = json.load(sys.stdin)
    if not isinstance(args, dict) or set(args) != {"text"} or not isinstance(args["text"], str):
        raise ValueError("expected an object containing only a string field: text")
    json.dump({"count": len(args["text"])}, sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")
except (ValueError, TypeError) as error:
    print(str(error), file=sys.stderr)
    sys.exit(1)
```
