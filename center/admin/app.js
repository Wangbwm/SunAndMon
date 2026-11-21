"use strict";

const byId = id => document.getElementById(id);
const edit = byId("edit");
const remove = byId("remove");
let key = "";
let nodes = [];
let showDeleted = false;
let selected = null;

function status(message, error = false) {
  const banner = byId("status");
  banner.textContent = message;
  banner.hidden = !message;
  banner.classList.toggle("error", error);
}

function setSignedIn(signedIn) {
  byId("gate").hidden = signedIn;
  byId("directory").hidden = !signedIn;
  byId("sign-out").hidden = !signedIn;
  byId("refresh").hidden = !signedIn;
}

async function request(path = "", method = "GET", body) {
  const response = await fetch("./api/nodes" + path, {
    method,
    headers: { Authorization: "Bearer " + key, "Content-Type": "application/json" },
    body: body === undefined ? undefined : JSON.stringify(body),
    cache: "no-store",
    credentials: "omit",
    redirect: "error",
    signal: AbortSignal.timeout(10000)
  });
  if (!response.ok) {
    let message = "请求失败，请稍后重试。";
    if (response.status === 401) message = "管理员密钥无效。";
    if (response.status === 503) message = "中心尚未启用节点管理。";
    if (response.status === 404) message = "该节点状态已变化，刷新后重试。";
    if (response.status === 400) message = "请检查名称、IP 与端口。";
    throw new Error(message);
  }
  return response.status === 204 ? null : response.json();
}

function textElement(tag, text, className = "") {
  const element = document.createElement(tag);
  element.textContent = text;
  if (className) element.className = className;
  return element;
}

function button(label, action, className = "") {
  const element = textElement("button", label, className);
  element.type = "button";
  element.addEventListener("click", action);
  return element;
}

function effective(node) {
  return { ...node.reported, ...node.override };
}

function render() {
  const count = nodes.filter(node => node.deleted).length;
  byId("active-count").textContent = nodes.length - count;
  byId("deleted-count").textContent = count;
  byId("active-filter").setAttribute("aria-pressed", String(!showDeleted));
  byId("deleted-filter").setAttribute("aria-pressed", String(showDeleted));
  const query = byId("query").value.trim().toLowerCase();
  const visible = nodes.filter(node => node.deleted === showDeleted).filter(node => {
    const { name, addresses = [] } = effective(node);
    return [node.id, name, node.reported.name, ...addresses].some(value => value?.toLowerCase().includes(query));
  });
  const rows = visible.map(node => {
    const { name, addresses = [], port, httpsPort } = effective(node);
    const row = document.createElement("tr");
    const identity = row.insertCell();
    identity.append(textElement("strong", name || "未命名节点", "node-name"),
                    textElement("div", node.id, "id"));
    const endpoints = row.insertCell();
    for (const address of addresses) {
      const host = address.includes(":") ? "[" + address + "]" : address;
      endpoints.append(textElement("span", host + ":" + port, "endpoint"));
    }
    if (httpsPort) endpoints.append(textElement("span", "HTTPS " + httpsPort, "endpoint id"));
    if (!addresses.length) endpoints.textContent = "—";
    row.insertCell().append(textElement("span", node.deleted ? "已删除" : node.online ? "在线" : "离线",
                                       node.online ? "node-state online" : "node-state"));
    const actions = document.createElement("div");
    actions.className = "actions";
    if (node.deleted) {
      actions.append(button("允许重新登记", async event => {
        event.currentTarget.disabled = true;
        try {
          await request("/" + encodeURIComponent(node.id) + "/restore", "POST");
          await reload("已允许重新登记。");
        } catch (error) {
          status(error.message, true);
          render();
        }
      }));
    } else {
      actions.append(button("编辑", () => showEdit(node)),
                     button("删除", () => showRemove(node), "remove"));
    }
    row.insertCell().append(actions);
    return row;
  });
  byId("nodes").replaceChildren(...rows);
  const empty = byId("empty");
  empty.hidden = rows.length !== 0;
  empty.textContent = query ? "没有匹配的节点。" : showDeleted ? "暂无已删除的节点。" : "暂无已登记的节点。";
}

async function reload(message = "") {
  nodes = await request();
  render();
  status(message);
}

function showEdit(node) {
  selected = node.id;
  byId("edit-id").textContent = node.id;
  const form = byId("edit-form");
  const values = node.override || {};
  form.elements.name.value = values.name || "";
  form.elements.name.placeholder = node.reported.name || "";
  form.elements.addresses.value = (values.addresses || []).join("\n");
  form.elements.addresses.placeholder = (node.reported.addresses || []).join("\n");
  for (const field of ["port", "httpsPort"]) {
    form.elements[field].value = values[field] || "";
    form.elements[field].placeholder = node.reported[field] || "";
  }
  form.querySelector(".error").hidden = true;
  edit.showModal();
}

function showRemove(node) {
  selected = node.id;
  byId("remove-name").textContent = effective(node).name || node.id;
  remove.querySelector(".error").hidden = true;
  remove.showModal();
}

async function save(form, method, message, body) {
  const dialog = form.closest("dialog");
  const errorText = form.querySelector(".error");
  const controls = [...form.querySelectorAll("button")];
  controls.forEach(control => { control.disabled = true; });
  errorText.hidden = true;
  try {
    await request("/" + encodeURIComponent(selected), method, body);
    dialog.close();
    try {
      await reload(message);
    } catch {
      status(message + " 列表刷新失败。", true);
    }
  } catch (error) {
    errorText.textContent = error.message;
    errorText.hidden = false;
  } finally {
    controls.forEach(control => { control.disabled = false; });
  }
}

byId("login").addEventListener("submit", async event => {
  event.preventDefault();
  const field = byId("key");
  if (!window.isSecureContext) {
    field.value = "";
    status("请使用 HTTPS 或本机回环地址打开管理页。", true);
    return;
  }
  const login = event.currentTarget.querySelector("button");
  key = field.value;
  field.value = "";
  login.disabled = true;
  try {
    await reload();
    setSignedIn(true);
    byId("query").focus();
  } catch (error) {
    key = "";
    status(error.message, true);
    field.focus();
  } finally {
    login.disabled = false;
  }
});
byId("sign-out").addEventListener("click", () => {
  key = "";
  nodes = [];
  showDeleted = false;
  byId("query").value = "";
  render();
  setSignedIn(false);
  status("");
  byId("key").focus();
});
byId("refresh").addEventListener("click", async event => {
  const control = event.currentTarget;
  control.disabled = true;
  try { await reload(); } catch (error) { status(error.message, true); }
  finally { control.disabled = false; }
});
byId("query").addEventListener("input", render);
byId("active-filter").addEventListener("click", () => { showDeleted = false; render(); });
byId("deleted-filter").addEventListener("click", () => { showDeleted = true; render(); });
for (const control of document.querySelectorAll("[data-close]")) {
  control.addEventListener("click", () => control.closest("dialog").close());
}
for (const dialog of [edit, remove]) {
  dialog.addEventListener("cancel", event => {
    if (dialog.querySelector("button[type=submit]").disabled) event.preventDefault();
  });
}
byId("edit-form").addEventListener("submit", event => {
  event.preventDefault();
  const form = event.currentTarget;
  save(form, "PATCH", "节点已保存。", {
    name: form.elements.name.value,
    addresses: form.elements.addresses.value.split(/\s+/).filter(Boolean),
    port: Number(form.elements.port.value),
    httpsPort: Number(form.elements.httpsPort.value)
  });
});
byId("remove-form").addEventListener("submit", event => {
  event.preventDefault();
  save(event.currentTarget, "DELETE", "节点已删除。");
});
