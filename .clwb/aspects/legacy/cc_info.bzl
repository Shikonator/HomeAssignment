# TEMPLATE-INCLUDE-BEGIN
load("@rules_cc//cc:defs.bzl", "CcInfo", "cc_common")

CC_USE_GET_TOOL_FOR_ACTION = True
# TEMPLATE-INCLUDE-END


CcInfoCompat = CcInfo
cc_common_compat = cc_common
