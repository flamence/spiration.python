# Spiration Python 支持

[简体中文](#) · [English](README.md)

## 快速开始

> 注意: 我们使用 Python 3.11.9 且同时可能随时更换版本。

在 [Spiration 文档](https://flamence.github.io/spiration.docs/) 中了解更多。

## PyPI 依赖管理

拓展可以在 `extension.json` 中声明 PyPI 依赖：

```json
{
    "python": {
        "requirements": ["openai>=3.37,<4"]
    }
}
```

在导入拓展入口脚本之前，加载器会用**同一套 CPython** 把这些依赖安装到拓展私有数据目录
（`<数据目录>/<拓展 ID>/python/site-packages`）：

```
python -m pip install --target <site-packages> -r <requirements.txt>
```

随后把该目录插入 `sys.path`。安装结果会被缓存：`python/state.txt` 记录解释器版本、
架构与需求列表，三者不变时直接复用，避免每次启动都访问网络。

- 依赖必须是带**版本约束**的普通 PEP 508 规格；pip 选项、URL 与本地路径会被拒绝。
- 首次安装需要能访问所配置的 PyPI 源（`PIP_*` 环境变量照常生效）；安装失败会拒绝
  装载该拓展，并把 pip 输出的尾部写入日志。
- 离线或打包部署可设置 `SPIRATION_PYTHON_DISABLE_PIP=1` 跳过自动安装，此时依赖需
  已预置在上面的目录中。
- `SPIRATION_PYTHON_PIP_TIMEOUT=<秒>` 可覆盖 pip 超时（默认 300 秒）。
- pip 子进程不显示控制台窗口，且运行期间释放 GIL，因此宿主界面只在首次安装时被阻塞。

## 拓展能力

脚本会收到注入的 `spiration` 模块。除了日志、路径、事件、服务以及可选的
`spiration.ui` 界面接口外，它还提供可选的 `spiration.agent` 接口
（`supported()`、`register_provider(name, provider)`、`unregister_provider(name)`），
用于向内置智能体提供对话补全 provider。

## 贡献支持

详见 [为 Spiration 贡献](https://flamence.github.io/spiration.docs/contributing)。

## 协议许可

该项目仓库使用 [Apache License 2.0](LICENSE) 协议。
