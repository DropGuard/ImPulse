## Context

core.cpp 的交互命令槽(interactiveCopy/Move 两个 ~70 行状态机、remove/trash/rename/copyCurrent/moveCurrent)全部被模态对话框挡住,813 行未覆盖。勘察确认:对话框是 **MW 的方法**(`showConfirmation` ×6 处、`fileReplaceDialog` ×4 处,core.cpp 经 `mw->` 调用),Core 自己只内联过 PrintDialog。接缝 = 提取这两方法的抽象接口,MW 以现有签名直接实现(零逻辑搬家),Core 经指针调用,测试注入带剧本的假件驱动状态机。saveAs/print/resize 的执行器本就 dialog-free(`resize(QSize)`、`saveCurrentFileAs(dest)` 可直测),不做接缝。文件重组(此前承诺的"白拿")经勘察确认**非必需**——1618 行/平均 16 行的宽浅 controller,接缝不迫使它。

## 设计

- **`gui/dialogs/dialogresult.h`**(新):`DialogResult` + `FileReplaceMode` 从 filereplacedialog.h 挪出(接口头不该拖 QDialog),filereplacedialog.h 改为 include。
- **`gui/iuserinteractions.h`**(新,仿 idirectoryview.h 的纯虚形状):
  ```cpp
  class IUserInteractions {
  public:
      virtual ~IUserInteractions() = default;
      virtual bool showConfirmation(const QString &title, const QString &msg) = 0;
      virtual DialogResult fileReplaceDialog(const QString &src, const QString &dst,
                                             FileReplaceMode mode, bool multiple) = 0;
  };
  ```
- **MW** 实现接口(`class MW : public FloatingWidgetContainer, public IUserInteractions`),两个方法体**原地不动**(mainwindow.cpp:918/612)。
- **Core**(core.h/core.cpp):新增 `IUserInteractions *ui_ = nullptr;`,`initGui()` 里 `ui_ = mw;`(主窗口就是生产环境的交互提供者——非隐藏默认,注释说明);10 处调用点 `mw->showConfirmation/fileReplaceDialog` → `ui_->…`。测试经既有 `friend class TestCoreNav` 直接改写 `core.ui_`,不加 setter。
- **`DialogResult` 语义是合同**:fake 返回的就是状态机吃的 DialogResult——`operator bool()` = yes(静默当 force 传给 copyFileTo/moveFileTo)、`all` 粘滞整个批次、`cancel` 逐层解锁、`yes` 在无 all 时每次问询后复位。测试钉的全部是这些现状语义。

## 提交划分

1. **接缝提交**(行为相邻重构):dialogresult.h + iuserinteractions.h + MW 实现接口 + Core ui_ + 10 处调用点。无罪证明 = 5 套件绿(同一对话框、同一答案路径)。
2. **测试提交**(core_nav_tests.cpp 新槽,复用其 friend/fork-per-function/夹具):假件 `FakeInteractions`(应答队列 + 调用计数 + 参数记录),驱动:
   - 单文件覆盖(剧本 yes)→ 内容落地、源保留;
   - **yes+all 粘滞**:一次应答,整批覆盖,后续零问询(调用计数断言);
   - **no+all = 跳过全部**:整批零问询零写入;
   - **逐文件重问**(yes 无 all):N 个碰撞 N 次问询(yes 复位语义);
   - **cancel 终止批次**:后续文件原封不动;
   - 目录覆盖文件分支(DIR_TO_FILE):yes 先删后拷 / no 跳过;
   - `removePermanent`/`moveToTrash`:确认 yes → 文件消失 / no → 保留(默认 confirmDelete=true 路径);
   - `renameCurrentSelection`:碰撞 + 剧本 confirm → 改名成功。
   发现真 bug 则按纪律 fix+pin。

## 验证

1. 每提交:`cmake --build build`(零警告)+ 5 套件。
2. 新测试按名单跑(纪律 #7)+ 全量 ctest;CORE_NAV 的 fork-per-function harness 自动给每个新测试独立 registry。
3. ASan 树一轮(命令面首次在 ASan 下执行,零新发现要求)。
4. `lint --gate`。