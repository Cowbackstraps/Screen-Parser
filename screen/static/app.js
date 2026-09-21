const elements = {
  analyzeButton: document.querySelector("#analyzeButton"),
  deviceStatus: document.querySelector("#deviceStatus"),
  deviceName: document.querySelector("#deviceName"),
  modelName: document.querySelector("#modelName"),
  pageType: document.querySelector("#pageType"),
  screenSize: document.querySelector("#screenSize"),
  screenStage: document.querySelector("#screenStage"),
  emptyState: document.querySelector("#emptyState"),
  imageWrap: document.querySelector("#imageWrap"),
  screenImage: document.querySelector("#screenImage"),
  overlay: document.querySelector("#overlay"),
  pageSummary: document.querySelector("#pageSummary"),
  pageTitle: document.querySelector("#pageTitle"),
  pageDescription: document.querySelector("#pageDescription"),
  nodeCount: document.querySelector("#nodeCount"),
  nodeList: document.querySelector("#nodeList"),
  nodeDetail: document.querySelector("#nodeDetail"),
  message: document.querySelector("#message"),
};

const state = { deviceId: null, analysis: null, selectedId: null };

function syncImageGeometry() {
  const analysis = state.analysis;
  if (!analysis || elements.imageWrap.hidden) return;

  const stageStyle = getComputedStyle(elements.screenStage);
  const horizontalPadding =
    parseFloat(stageStyle.paddingLeft) + parseFloat(stageStyle.paddingRight);
  const verticalPadding =
    parseFloat(stageStyle.paddingTop) + parseFloat(stageStyle.paddingBottom);
  const availableWidth = Math.max(1, elements.screenStage.clientWidth - horizontalPadding);
  const stageHeight = Math.max(1, elements.screenStage.clientHeight - verticalPadding);
  const viewportHeight = Math.max(320, window.innerHeight - 240);
  const availableHeight = Math.min(stageHeight, viewportHeight);
  const scale = Math.min(
    availableWidth / analysis.screen_width,
    availableHeight / analysis.screen_height,
  );

  elements.imageWrap.style.width = `${analysis.screen_width * scale}px`;
  elements.imageWrap.style.height = `${analysis.screen_height * scale}px`;
}

async function loadStatus() {
  try {
    const response = await fetch("/api/status");
    const data = await response.json();
    state.deviceId = data.default_device;
    elements.deviceName.textContent = state.deviceId || "未连接";
    elements.modelName.textContent = data.model || "未配置";
    elements.deviceStatus.textContent = state.deviceId ? "HDC 已连接" : "未检测到设备";
    elements.deviceStatus.className = `status ${state.deviceId ? "ready" : "error"}`;
    elements.analyzeButton.disabled = !state.deviceId;
  } catch (error) {
    showError(`状态读取失败：${error.message}`);
    elements.analyzeButton.disabled = true;
  }
}

async function analyzeScreen() {
  setLoading(true);
  try {
    const response = await fetch("/api/analyze", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ device_id: state.deviceId }),
    });
    const data = await response.json();
    if (!response.ok) throw new Error(data.error || "解析请求失败");
    state.analysis = data;
    state.deviceId = data.device_id;
    state.selectedId = null;
    renderAnalysis();
    return data;
  } catch (error) {
    showError(error.message);
    throw error;
  } finally {
    setLoading(false);
  }
}

function setLoading(loading) {
  elements.analyzeButton.disabled = loading;
  elements.analyzeButton.textContent = loading ? "解析中…" : "解析";
  elements.screenStage.classList.toggle("is-loading", loading);
  if (loading && !state.analysis) {
    elements.emptyState.hidden = false;
    elements.emptyState.querySelector("strong").textContent = "正在理解当前屏幕";
    elements.emptyState.querySelector("p").textContent = "正在截图并等待远程 VLM 返回。";
    elements.message.hidden = false;
    elements.message.className = "message";
    elements.message.textContent = "解析中，请稍候…";
  }
}

function renderAnalysis() {
  const analysis = state.analysis;
  elements.emptyState.hidden = true;
  elements.imageWrap.hidden = false;
  elements.screenImage.src = analysis.screenshot;
  syncImageGeometry();
  elements.screenSize.textContent = `${analysis.screen_width} × ${analysis.screen_height}`;
  elements.deviceName.textContent = analysis.device_id;
  elements.pageType.textContent = analysis.page.page_type || "unknown";
  elements.pageTitle.textContent = analysis.page.title || analysis.current_app;
  elements.pageDescription.textContent = analysis.page.summary || "暂无页面摘要";
  elements.pageSummary.hidden = false;
  elements.nodeCount.textContent = `${analysis.nodes.length} 个`;
  elements.message.hidden = analysis.nodes.length > 0;
  elements.message.className = "message";
  elements.message.textContent = "没有识别到有效节点。";
  renderNodes();
}

function renderNodes() {
  const nodes = state.analysis?.nodes || [];
  elements.overlay.replaceChildren();
  elements.nodeList.replaceChildren();

  nodes.forEach((node, index) => {
    const bounds = node.bounds;
    const box = document.createElement("button");
    box.type = "button";
    box.className = `node-box ${node.evidence === "pending" ? "pending" : ""}`;
    box.dataset.nodeId = node.id;
    box.setAttribute("aria-label", `${index + 1}. ${node.label}`);
    box.style.left = `${bounds.x1 / 10}%`;
    box.style.top = `${bounds.y1 / 10}%`;
    box.style.width = `${(bounds.x2 - bounds.x1) / 10}%`;
    box.style.height = `${(bounds.y2 - bounds.y1) / 10}%`;
    box.innerHTML = `<span class="node-index">${index + 1}</span>`;
    box.addEventListener("click", () => selectNode(node.id));
    elements.overlay.append(box);

    const item = document.createElement("li");
    item.className = `node-row ${node.evidence === "pending" ? "pending" : ""}`;
    const button = document.createElement("button");
    button.type = "button";
    button.dataset.nodeId = node.id;
    button.innerHTML = `
      <span class="index">${String(index + 1).padStart(2, "0")}</span>
      <span class="node-copy">
        <span class="node-label"></span>
        <span class="node-role"></span>
      </span>
      <span class="confidence">${Math.round(node.confidence * 100)}%</span>`;
    button.querySelector(".node-label").textContent = node.label;
    button.querySelector(".node-role").textContent = `${node.role}${node.interactive ? " · 可交互" : ""}`;
    button.addEventListener("click", () => selectNode(node.id));
    item.append(button);
    elements.nodeList.append(item);
  });
}

function selectNode(nodeId) {
  state.selectedId = nodeId;
  document.querySelectorAll("[data-node-id]").forEach((item) => {
    item.classList.toggle("selected", item.dataset.nodeId === nodeId);
  });
  const node = state.analysis.nodes.find((item) => item.id === nodeId);
  if (!node) return;
  const b = node.bounds;
  elements.nodeDetail.hidden = false;
  elements.nodeDetail.replaceChildren();
  const title = document.createElement("strong");
  title.textContent = node.label;
  const detail = document.createElement("div");
  detail.textContent = `${node.description || "无补充描述"} · 坐标 [${b.x1}, ${b.y1}, ${b.x2}, ${b.y2}]`;
  elements.nodeDetail.append(title, detail);
}

function showError(message) {
  elements.message.hidden = false;
  elements.message.className = "message error";
  elements.message.textContent = message;
  elements.deviceStatus.textContent = "解析失败";
  elements.deviceStatus.className = "status error";
}

elements.analyzeButton.addEventListener("click", () => {
  analyzeScreen().catch(() => {});
});
loadStatus();

function registerWebMcpTool() {
  const context = document.modelContext;
  if (!context?.registerTool) return;

  try {
    Promise.resolve(
      context.registerTool({
        name: "analyze_current_harmony_screen",
        title: "解析当前鸿蒙屏幕",
        description: "截取当前 HDC 模拟器页面，使用远程视觉模型分析，并在页面中显示结构化语义节点。",
        inputSchema: { type: "object", properties: {}, additionalProperties: false },
        annotations: { readOnlyHint: true, untrustedContentHint: true },
        async execute() {
          if (!state.deviceId) throw new Error("未检测到 HDC 设备或模拟器");
          const analysis = await analyzeScreen();
          return {
            device_id: analysis.device_id,
            page: analysis.page,
            node_count: analysis.nodes.length,
          };
        },
      }),
    ).catch(() => {});
  } catch (_) {
    // WebMCP is optional and unsupported browsers keep the normal button flow.
  }
}

registerWebMcpTool();

if ("ResizeObserver" in window) {
  new ResizeObserver(syncImageGeometry).observe(elements.screenStage);
} else {
  window.addEventListener("resize", syncImageGeometry);
}
