export function normalizeBssid(value) {
    return typeof value === 'string' ? value.trim().toUpperCase() : '';
}

export function isSameAccessPoint(left, right) {
    if (!left || !right) return false;
    if (left === right) return true;
    const leftBssid = normalizeBssid(left.bssid);
    const rightBssid = normalizeBssid(right.bssid);
    return Boolean(leftBssid && rightBssid && leftBssid === rightBssid);
}

export function filterScanResults(networks, query) {
    const normalizedQuery = String(query || '').trim().toLowerCase();
    if (!normalizedQuery) return networks;
    return networks.filter(network => String(network.ssid || '').toLowerCase().includes(normalizedQuery));
}

export function selectScanResult(currentSelection, nextSelection) {
    return {
        selection: nextSelection,
        clearPassword: !isSameAccessPoint(currentSelection, nextSelection),
    };
}

export function reconcileScanRefresh(currentSelection, nextNetworks, succeeded = true) {
    if (!succeeded) {
        return {
            selection: currentSelection,
            outcome: 'unchanged',
            clearBssid: false,
            clearPassword: false,
        };
    }
    if (!currentSelection) {
        return {
            selection: null,
            outcome: 'none',
            clearBssid: false,
            clearPassword: false,
        };
    }

    const selectedBssid = normalizeBssid(currentSelection.bssid);
    const match = selectedBssid
        ? nextNetworks.find(network => normalizeBssid(network.bssid) === selectedBssid)
        : null;
    if (match) {
        return {
            selection: match,
            outcome: 'rebound',
            clearBssid: false,
            clearPassword: !match.secure,
        };
    }
    return {
        selection: null,
        outcome: 'manual',
        clearBssid: true,
        clearPassword: false,
    };
}

export function shouldInvalidateScanSelection(selection, currentSsid, currentBssid) {
    if (!selection) return false;
    return String(currentSsid) !== String(selection.ssid || '')
        || normalizeBssid(currentBssid) !== normalizeBssid(selection.bssid);
}

export function channelForSelection(selection) {
    if (!selection) return 0;
    const channel = Number(selection.channel);
    return Number.isInteger(channel) && channel > 0 ? channel : 0;
}

export function connectionFieldsForSelection(selection, currentBssid) {
    const bssid = String(currentBssid || '').trim();
    return {
        bssid: bssid || undefined,
        channel: channelForSelection(selection),
    };
}
