/**
 * @file      assert.h
 * @brief     Test assert code.
 * @author    Martin Kelly <mkelly@xevo.com>
 * @copyright Copyright (C) 2019 Xevo Inc. All Rights Reserved.
 */

#ifndef HOUND_TEST_ASSERT_H_
#define HOUND_TEST_ASSERT_H_

#include <hound/hound.h>
#include <xlib/xassert.h>

#define XASSERT_ERRCODE(x, y) _XASSERT_ERRCODE(x, y, hound_strerror)
#define XASSERT_OK(err) XASSERT_ERRCODE(err, HOUND_OK)

#endif /* HOUND_TEST_ASSERT_H_ */
