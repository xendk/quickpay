<?php

/**
 * @file
 * Contains Payment hooks.
 */

/**
 * Implements hook_payment_method_controller_info().
 */
function payment_quickpay_payment_method_controller_info() {
  return array(
    'QuickpayPaymentMethodController',
  );
}
